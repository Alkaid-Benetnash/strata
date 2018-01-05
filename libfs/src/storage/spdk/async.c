#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <errno.h>

#include "common.h"
#include "async.h"
#include "global/defs.h"
#include "global/mem.h"
#include "global/util.h"

#define Q_DEPTH 512
//#define Q_DEPTH 64

//#define INNER_IO_SIZE (4U << 20);
//#define INNER_IO_SIZE (512 << 10);
//#define INNER_IO_SIZE (64 << 10);
//#define INNER_IO_SIZE (16 << 10);
#define INNER_IO_SIZE (4 << 10);

struct readahead {
	uint64_t blockno;
	uint32_t size;
	uint8_t *ra_buf;
};

struct spdk_async_data {
#ifdef CONCURRENT
  unsigned int issued;
#else
	unsigned int read_issued;
	unsigned int write_issued;
	unsigned int ra_issued;
#endif
  unsigned int inner_io_size;
};

struct spdk_async_io {
	void* user_arg;
	void(* user_cb)(void*);
	int ios_left;
	char *buffer;
	char *guest_buffer;
	uint64_t start, len;
};

//#define pthread_mutex_lock(m) do { printf("[%d] TRY LOCK @ %d\n", qpair_idx(), __LINE__); pthread_mutex_lock(m); printf("LOCK\n"); } while (0)
//#define pthread_mutex_unlock(m) do { printf("[%d] TRY UNLOCK @ %d\n", qpair_idx(), __LINE__); pthread_mutex_unlock(m); printf("UNLOCK\n"); } while (0)

#ifndef CONCURRENT
// (iangneal): Only use a special readahead qpair if we're not already making a
// bunch of parallel qpairs.
static struct spdk_nvme_qpair *read_qpair;
#endif
// (iangneal): These structures are per qpair.
static struct readahead *ra;
static struct spdk_async_data *async_data;
static uint8_t *do_readahead;
static uint8_t **write_buffer;
static uint8_t **read_buffer;
static uint32_t *write_buffer_pointer;
static uint32_t *read_buffer_pointer;

#ifdef CONCURRENT
static pthread_mutex_t completion_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

/**********************************
 *
 *      GENERAL FUNCTIONS
 *
 **********************************/

int spdk_process_completions(int read)
{
  int r = 0, idx = qpair_idx();
	struct ns_entry *ns_entry = g_namespaces;
#ifndef CONCURRENT
  struct spdk_nvme_qpair *qpair;
	if (read)
		qpair = read_qpair;
	else
		qpair = ns_entry->qpairs[0];

	r = spdk_nvme_qpair_process_completions(qpair, 0);
	//if not an error
	if (r > 0) {
		if (read)
			async_data[idx].read_issued -= r;
		else
			async_data[idx].write_issued -= r;

		//printf("completed %d, oustanding: %d\n", r, async_data.issued);
	}
#else
  int total = 0;
  pthread_mutex_lock(&completion_mutex);
  // iangneal: check every qpair.
  for (int i = 0; i < ns_entry->nqpairs; ++i) {
    pthread_mutex_lock(ns_entry->qtexs[i]);
    r = spdk_nvme_qpair_process_completions(ns_entry->qpairs[i], 0);
    pthread_mutex_unlock(ns_entry->qtexs[i]);
    if (r >= 0) {
      async_data[idx].issued -= r;
      total += r;
    } else {
      printf("completion error %d\n", r);
    }
  }
  r = total;
  pthread_mutex_unlock(&completion_mutex);
#endif
	return r;
}

void spdk_wait_completions(int read)
{
	struct ns_entry *ns_entry = g_namespaces;
	unsigned long nr_io_waits = 0;

	/* Waiting all outstanding IOs */
#ifndef CONCURRENT
  struct spdk_nvme_qpair *qpair;
	if (read) {
		nr_io_waits = async_data[idx].read_issued;
		qpair = read_qpair;
	}
	else {
		nr_io_waits = async_data[idx].write_issued;
		qpair = ns_entry->qpairs[idx];
	}

	/* Waiting all outstanding IOs */
	while (nr_io_waits != 0) {
		int r = spdk_nvme_qpair_process_completions(qpair, 0);
		if (r > 0) {
			if (read)
				async_data[idx].read_issued -= r;
			else
				async_data[idx].write_issued -= r;

			nr_io_waits -= r;

		} else if (r < 0) {
			printf("completion error %d\n", r);
		}
	}
#else
  pthread_mutex_lock(&completion_mutex);

  // pre-lock all the qpairs.
  for (int i = 0; i < ns_entry->nqpairs; ++i)
    pthread_mutex_lock(ns_entry->qtexs[i]);

  for (int i = 0; i < ns_entry->nqpairs; ++i) {
    nr_io_waits += async_data[i].issued;
  }

  while (nr_io_waits > 0) {
    // iangneal: check every qpair.
    for (int i = 0; i < ns_entry->nqpairs; ++i) {
      int r = spdk_nvme_qpair_process_completions(ns_entry->qpairs[i], 0);
      if (r >= 0) {
        async_data[i].issued -= r;
      } else {
        printf("completion error %d\n", r);
      }

      nr_io_waits -= r;
    }
  }

  // unlock all the qpairs, reverse order
  for (int i = ns_entry->nqpairs - 1; i >= 0; --i)
    pthread_mutex_unlock(ns_entry->qtexs[i]);

  pthread_mutex_unlock(&completion_mutex);
#endif
}

void spdk_async_io_exit(void)
{
	libspdk_exit();
}

unsigned int spdk_async_get_n_lbas(void)
{
	return libspdk_get_n_lbas();
}

int spdk_async_io_init(void)
{
	int ret, i, n;

  printf("Intializing spdk async engine\n");

	ret = libspdk_init();
	if (ret < 0)
		panic("cannot initialize libspdk\n");

#ifndef CONCURRENT
	// allocate separate q_pair for readahead
	read_qpair = spdk_nvme_ctrlr_alloc_io_qpair(g_namespaces->ctrlr, NULL, 0);
	if (!read_qpair)
		panic("cannot allocate qpair\n");
#endif

  n = g_namespaces->nqpairs;

  ra = mlfs_alloc(n * sizeof(*ra));
  async_data = mlfs_alloc(n * sizeof(*async_data));
  do_readahead = mlfs_alloc(n * sizeof(*do_readahead));
  write_buffer = mlfs_alloc(n * sizeof(*write_buffer));
  read_buffer = mlfs_alloc(n * sizeof(*read_buffer));
  write_buffer_pointer = mlfs_alloc(n * sizeof(write_buffer_pointer));
  read_buffer_pointer = mlfs_alloc(n * sizeof(read_buffer_pointer));

  for (i = 0; i < n; ++i) {
    async_data[i].inner_io_size = INNER_IO_SIZE;
#ifdef CONCURRENT
    async_data[i].issued = 0;
#else
    async_data[i].read_issued = 0;
    async_data[i].write_issued = 0;
    async_data[i].ra_issued = 0;
#endif

    ra[i].ra_buf = spdk_dma_zmalloc((2 << 20), 0x1000, NULL);

    read_buffer_pointer[i] = 0;
    write_buffer_pointer[i] = 0;

    read_buffer[i] = spdk_dma_zmalloc((6 << 20), 0x1000, NULL);
    write_buffer[i] = spdk_dma_zmalloc((6 << 20), 0x1000, NULL);
  }

	return 0;
}

/**********************************
 *
 *      READ FUNCTIONS
 *
 **********************************/

static int spdk_readahead_completions(void)
{
  int idx = qpair_idx();
	//printf("RA completion (before) %d\n", async_data.ra_issued);
#ifdef CONCURRENT
  pthread_mutex_lock(g_namespaces->qtexs[idx]);
	while (async_data[idx].issued != 0) {
#else
	while (async_data[idx].ra_issued != 0) {
#endif
#ifdef CONCURRENT
		int r = spdk_nvme_qpair_process_completions(g_namespaces->qpairs[idx], 0);
#else
		int r = spdk_nvme_qpair_process_completions(read_qpair, 0);
#endif
		if (r > 0) {
			//printf("completion %d\n", r);
			//fflush(stdout);
#ifdef CONCURRENT
			async_data[idx].issued -= r;
#else
			async_data[idx].ra_issued -= r;
#endif
		}
	}
#ifdef CONCURRENT
  pthread_mutex_unlock(g_namespaces->qtexs[idx]);
#endif

	//printf("RA completion (after) %d\n", async_data.ra_issued);
	//return r;
	return 0;
}

static void spdk_async_readahead_callback(void *arg,
		const struct spdk_nvme_cpl *completion)
{
	struct spdk_async_io *io = arg;

	io->ios_left -= 1;

	//printf("ra done %lu %d\n", ra.blockno, io->ios_left);

	io->ios_left -= 1;

	if(io->ios_left == 0) {
		if(io->user_cb) {
			(*(io->user_cb))(io->user_arg);
		}
	}

	mlfs_free(arg);
}

int spdk_async_readahead(unsigned long blockno, unsigned int io_size)
{
	// blockno is reserved for future
  int rc = 0, idx = qpair_idx();
	struct ns_entry *ns_entry = g_namespaces;
	unsigned int i;
	static struct spdk_async_io *ra_io = NULL;

#ifdef CONCURRENT
    pthread_mutex_lock(ns_entry->qtexs[idx]);
#endif
	int n_ios = ceil((float)io_size/(float)async_data[idx].inner_io_size);

	if (!do_readahead[idx])
		goto end;

	ra[idx].blockno = blockno;
	ra[idx].size = io_size;

	for (i = 0; i < io_size; i += async_data[idx].inner_io_size) {
		int to_read = io_size - i < async_data[idx].inner_io_size ?
			io_size - i : async_data[idx].inner_io_size;
		int inner_blocks = ceil(to_read/BLOCK_SIZE);
		unsigned long to_block = blockno+(i/BLOCK_SIZE);

		ra_io = mlfs_alloc(sizeof(struct spdk_async_io));
		ra_io->buffer = ra[idx].ra_buf + i;

		ra_io->user_arg = NULL;
		ra_io->user_cb = NULL;
		ra_io->ios_left = n_ios;

		ra_io->start = i;
		ra_io->len = to_read;

		//printf("Issuing readahead of %d to block %d\n", to_block, inner_blocks);
		if(spdk_nvme_ns_cmd_read(
					ns_entry->ns,
#ifdef CONCURRENT
          ns_entry->qpairs[idx],
#else
					read_qpair,
#endif
					ra_io->buffer,
					to_block, /* LBA start */
					inner_blocks, /* number of LBAs */
					spdk_async_readahead_callback, ra_io, 0) != 0) {
			fprintf(stderr, "starting write I/O failed\n");
      rc = -1;
      goto end;
		}

#ifdef CONCURRENT
		async_data[idx].issued++;
#else
    async_data[idx].ra_issued++;
#endif
  }

end:
#ifdef CONCURRENT
  pthread_mutex_unlock(ns_entry->qtexs[idx]);
#endif
	return rc;
}

static void spdk_async_io_read_callback(void *arg,
		const struct spdk_nvme_cpl *completion)
{
	uint64_t start_tsc = 0;
	struct spdk_async_io *io = arg;

	mlfs_debug("copy to user_buffer start %d len %d\n", io->start, io->len);

	if (g_enable_perf_stats)
		start_tsc = asm_rdtscp();

	memcpy(io->guest_buffer+io->start, io->buffer + io->start, io->len);

	if (g_enable_perf_stats)
		g_spdk_perf_stats.memcpy_tsc += (asm_rdtscp() - start_tsc);

	io->ios_left -= 1;

	mlfs_debug("Had %d ios left, now its %d\n",  io->ios_left+1,  io->ios_left);
	if(io->ios_left == 0) {
		if(io->user_cb) {
			(*(io->user_cb))(io->user_arg);
		}
	}

	mlfs_free(arg);
}

/*
 * io size is in bytes. blockno is the actual block we want to read
 * cannot assume guest buffer is in pinned memory, so need a copy
 */
int spdk_async_io_read(uint8_t *guest_buffer, unsigned long blockno,
		uint32_t bytes_to_read, void(* cb)(void*), void* arg)
{
	uint32_t i;
	int n_blocks;
	struct ns_entry *ns_entry = g_namespaces;
	struct spdk_async_io* read_io;
  int idx = qpair_idx();
  int rc = 0;

#ifdef CONCURRENT
  pthread_mutex_lock(ns_entry->qtexs[idx]);
#endif
	//how many ios we need to submit
	int n_ios = ceil((float)bytes_to_read/(float)async_data[idx].inner_io_size);

	/* check whether data can be served from readahead buffer */
	if (blockno >= ra[idx].blockno &&
		(blockno + (bytes_to_read >> g_block_size_shift) <=
		 (ra[idx].blockno) + (ra[idx].size >> g_block_size_shift))) {
		uint32_t ra_offset;

		ra_offset = (blockno - ra[idx].blockno) << g_block_size_shift;

		// check whether it can get data from readahead buffer.
		if (do_readahead[idx])
			spdk_readahead_completions();

		do_readahead[idx] = 0;

		memcpy(guest_buffer, ra[idx].ra_buf + ra_offset, bytes_to_read);

		mlfs_debug("Get from RA buffer: req=%lu-%lu, ra=%lu-%lu\n",
				blockno, (bytes_to_read >> 12),
				ra[idx].blockno, (ra[idx].size >> 12));

    rc = bytes_to_read;
    goto end;
	}

	do_readahead[idx] = 1;

	mlfs_debug("RA is not available: req=%lu-%lu, ra=%lu-%lu\n",
			blockno, (bytes_to_read >> 12),
			ra[idx].blockno, (ra[idx].size >> 12));

	// If it won't fit all, don't issue any
	//TODO: possibly read as many bytes as we can and return this amount
	if(n_ios > Q_DEPTH) {
	  errno = EFBIG;
	  rc = -1;
    goto end;
	}
#ifdef CONCURRENT
	if(async_data[idx].issued + n_ios > Q_DEPTH) {
#else
	if(async_data[idx].read_issued + n_ios > Q_DEPTH) {
#endif
		errno = EBUSY;
		rc = -1;
    goto end;
	}

	if (bytes_to_read < g_block_size_bytes)
		n_blocks = 1;
	else {
		n_blocks = bytes_to_read >> g_block_size_shift;
		if (bytes_to_read % g_block_size_bytes)
			n_blocks++;
	}

	for (i = 0; i < bytes_to_read; i += async_data[idx].inner_io_size) {
		//min (io_size, remaining bytes)
		int to_read = bytes_to_read - i < async_data[idx].inner_io_size ?
			bytes_to_read - i : async_data[idx].inner_io_size;
		int inner_blocks = ceil(to_read/BLOCK_SIZE);
		unsigned long to_block = blockno+(i/BLOCK_SIZE);

		read_io = mlfs_alloc(sizeof(struct spdk_async_io));
		read_io->buffer = read_buffer[idx] + read_buffer_pointer[idx];

		read_io->user_arg = arg;
		read_io->user_cb = cb;
		read_io->guest_buffer = guest_buffer;
		read_io->ios_left = n_ios;

		read_io->start = i;
		read_io->len = to_read;

		mlfs_debug("Issuing read of %d to block %d\n", to_block, inner_blocks);
		if(spdk_nvme_ns_cmd_read(
					ns_entry->ns,
#ifdef CONCURRENT
					ns_entry->qpairs[idx],
#else
					read_qpair,
#endif
          read_io->buffer + i,
					to_block, /* LBA start */
					inner_blocks, /* number of LBAs */
					spdk_async_io_read_callback, read_io, 0) != 0) {
			fprintf(stderr, "starting write I/O failed\n");
			rc = -1;
      goto end;
		}
		//could add all at once, but this will prevent infinite waiting in case
		//one write fails
#ifdef CONCURRENT
		async_data[idx].issued++;
#else
    async_data[idx].read_issued++;
#endif
	}

	read_buffer_pointer[idx] += i;
	read_buffer_pointer[idx] = (read_buffer_pointer[idx] % (6 << 20));

  rc = bytes_to_read;

end:
#ifdef CONCURRENT
  pthread_mutex_unlock(ns_entry->qtexs[idx]);
#endif
  return rc;
}

/**********************************
 *
 *      WRITE FUNCTIONS
 *
 **********************************/
static void spdk_async_io_write_callback(void *arg,
		const struct spdk_nvme_cpl *completion)
{
	struct spdk_async_io *io = arg;
	//decrement how many ios are left
	io->ios_left -= 1;

	mlfs_debug("Had %d ios left, now its %d\n", io->ios_left+1, io->ios_left);
	//if we were the last one, finish and cleanup
	if(io->ios_left == 0) {
		if(io->user_cb) {
			(*(io->user_cb))(io->user_arg);
		}
		mlfs_free(arg);
	}

	mlfs_debug("Write callback done\n");
}

int spdk_async_io_write(uint8_t *guest_buffer, unsigned long blockno,
		uint32_t bytes_to_write, void(* cb)(void*), void* arg)
{
	struct spdk_async_io* io;
	uint64_t start_tsc = 0;
	int n_blocks;
	struct ns_entry *ns_entry = g_namespaces;
	unsigned int i;
  int idx = qpair_idx(), rc = -1;

#ifdef CONCURRENT
  pthread_mutex_lock(ns_entry->qtexs[idx]);
#endif
	//how many ios we need to submit
	int n_ios = ceil((float)bytes_to_write/(float)async_data[idx].inner_io_size);

	//if it wont fit all, dont issue any
	//TODO: possibly write as many bytes as we can and return this amount
	if(n_ios > Q_DEPTH) {
		errno = EFBIG;
		goto end;
	}

#ifdef CONCURRENT
  if(async_data[idx].issued + n_ios > Q_DEPTH) {
#else
	if(async_data[idx].write_issued + n_ios > Q_DEPTH) {
#endif
    errno = EBUSY;
		goto end;
	}

	//n_blocks = ceil(bytes_to_write/(double)BLOCK_SIZE);
	if (bytes_to_write < g_block_size_bytes)
		n_blocks = 1;
	else {
		n_blocks = bytes_to_write >> g_block_size_shift;
		if (bytes_to_write % g_block_size_bytes)
			n_blocks++;
	}

	io = mlfs_alloc(sizeof(struct spdk_async_io));
	io->user_arg = arg;
	io->user_cb = cb;
	io->buffer = write_buffer[idx] + write_buffer_pointer[idx];
	mlfs_debug("%d / %d    = ios left %d\n",
			bytes_to_write, async_data[idx].inner_io_size, n_ios);
	io->ios_left = n_ios;

	//this memcpy segfaults if we have too long of a queue
	//and large io size because we couldnt alloc that many bytes
	if (g_enable_perf_stats)
		start_tsc = asm_rdtscp();

	memcpy(io->buffer, guest_buffer, bytes_to_write);

	if (g_enable_perf_stats)
		g_spdk_perf_stats.memcpy_tsc += (asm_rdtscp() - start_tsc);

	for (i = 0; i < bytes_to_write; i += async_data[idx].inner_io_size) {
		//min (io_size, remaining bytes)
		int to_write = bytes_to_write - i < async_data[idx].inner_io_size ?
			bytes_to_write - i : async_data[idx].inner_io_size;
		int inner_blocks = ceil(to_write/BLOCK_SIZE);
		int to_block = blockno+(i/BLOCK_SIZE);

		mlfs_debug("Issuing write of %d to block %d\n", to_block, inner_blocks);
		if(spdk_nvme_ns_cmd_write(
					ns_entry->ns,
					ns_entry->qpairs[idx],
					io->buffer + i,
					to_block, /* LBA start */
					inner_blocks, /* number of LBAs */
					spdk_async_io_write_callback, io, 0) != 0) {
			fprintf(stderr, "starting write I/O failed\n");
			goto end;
		}
		//could add all at once, but this will prevent infinite waiting in case
		//one write fails
#ifdef CONCURRENT
		async_data[idx].issued++;
#else
    async_data[idx].write_issued++;
#endif
  }

	write_buffer_pointer[idx] += i;
	write_buffer_pointer[idx] = (write_buffer_pointer[idx] % (6 << 20));

  rc = bytes_to_write;

end:
#ifdef CONCURRENT
  pthread_mutex_unlock(ns_entry->qtexs[idx]);
#endif
  return rc;
}

static void spdk_sync_io_trim_callback(void *arg,
		const struct spdk_nvme_cpl *completion)
{
}

int spdk_io_trim(unsigned long blockno, unsigned int n_bytes)
{
	int ret;
  int idx = qpair_idx();
	struct spdk_nvme_dsm_range ranges[256];
	struct ns_entry *ns_entry = g_namespaces;

	uint32_t blocks_left = ceil(n_bytes/(double)BLOCK_SIZE);
	uint32_t max_range = 1 << 16;
	uint32_t start_block = blockno;
	uint32_t count = 0;

	while (blocks_left > 0) {
		int blocks;
		if(blocks_left >= max_range) {
			blocks = max_range;
		} else {
			blocks = blocks_left;
		}
		blocks_left -= blocks;

		ranges[count].starting_lba = start_block;
		ranges[count].length = blocks;
		ranges[count].attributes.raw = 0;
		count++;

		start_block += blocks;
	}

	//printf("Issuing %d ranges trim\n", count);
#ifdef CONCURRENT
  pthread_mutex_lock(ns_entry->qtexs[idx]);
#endif
	ret = spdk_nvme_ns_cmd_dataset_management(ns_entry->ns,
                                            ns_entry->qpairs[idx],
                                            SPDK_NVME_DSM_ATTR_DEALLOCATE,
                                            ranges,
                                            count,
                                            spdk_sync_io_trim_callback, NULL);

	if (ret != 0) {
		printf("cannot issue dataset command\n");
		exit(-1);
	}

	// Wait until it's done.
	while(!spdk_nvme_qpair_process_completions(ns_entry->qpairs[idx], 0));

#ifdef CONCURRENT
  pthread_mutex_unlock(ns_entry->qtexs[idx]);
#endif

	return 0;
}
