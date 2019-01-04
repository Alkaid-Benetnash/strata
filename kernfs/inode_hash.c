#ifdef HASHTABLE

#include <stdbool.h>
#include "inode_hash.h"


#define GPOINTER_TO_UINT(x) ((uint64_t)x)

mlfs_fsblk_t single_hash_meta_loc = 0;
mlfs_fsblk_t chunk_hash_meta_loc = 0;
mlfs_fsblk_t id_map_meta_loc = 0;

// (iangneal): Global hash table for all of NVRAM. Each inode has a point to
// this one hash table just for abstraction of the inode interface.
static GHashTable *ghash = NULL;
// (iangneal): Second level hash table.
static GHashTable *gsuper = NULL;

static pthread_mutex_t alloc_tex = PTHREAD_MUTEX_INITIALIZER;
/*
 *
 */
void init_hash(struct super_block *sb) {

  if (ghash) return;

  assert(sb);

  printf("Initializing NVM hash table structures...\n");

  // Calculate the locations of all the data structure metadata.
  single_hash_meta_loc = sb->ondisk->ndatablocks - 1;
  chunk_hash_meta_loc = single_hash_meta_loc - 1;
  id_map_meta_loc = chunk_hash_meta_loc - 1;

  // single block table
  ghash = g_hash_table_new(g_direct_hash, sb->ondisk->ndatablocks, 1,
      single_hash_meta_loc);
  if (!ghash) {
    panic("Failed to initialize the single-block hash table.\n");
  }

  printf("Finished initializing the single-block hash table.\n");

  // chunk (maps a range of blocks) hash table
  gsuper = g_hash_table_new(g_direct_hash, sb->ondisk->ndatablocks,
      RANGE_SIZE, chunk_hash_meta_loc);
  if (!gsuper) {
    panic("Failed to initialize multi-block hash table\n");
  }

  printf("Finished initializing the multi-block hash table.\n");
  printf("Finished initializing NVM hash table structures.\n");

}

int insert_hash(GHashTable *hash, struct inode *inode, hash_key_t key,
    hash_value_t value, hash_value_t size) {
  int ret = 0;

  // if not exists, then the value was not already in the table, therefore
  // success.

  return g_hash_table_insert(hash, key, value, size);
}

/*
 * Returns 0 if not found (value == 0 means no associated value).
 */
int lookup_hash(struct inode *inode, mlfs_lblk_t key, hash_value_t* value,
    hash_value_t *size, hash_value_t *index) {
  int ret = 0;
  hash_key_t k = MAKEKEY(inode, key);
  hash_key_t r = RANGE_KEY(inode->inum, key);

  *index = 0;
  // Two-level lookup
  g_hash_table_lookup(gsuper, r, value, size);
  //printf("%u -> %lu, %lu %lu\n", key, r, *value, *size);
  bool present = (*value) && ((key & RANGE_BITS) < *size);
  //printf("--- %lu & %lu (%lu) < %lu\n", key, RANGE_BITS, key & RANGE_BITS, *size);
  if (!present) {
    //printf("Not in big.\n");
    g_hash_table_lookup(ghash, k, value, size);
    present = *value != 0;
    //if (!present) printf("Not in small.\n");
  } else {
    *index = (key & RANGE_BITS);
  }

  return (int) present;
}

/*
 * Returns FALSE if the requested logical block was not present in any of the
 * two hash tables.
 */
int erase_hash(struct inode *inode, mlfs_lblk_t key) {
  int ret = 0;
  hash_key_t k = MAKEKEY(inode, key);
  hash_key_t r = RANGE_KEY(inode->inum, key);

  if (!g_hash_table_remove(gsuper, r)) {
    return g_hash_table_remove(ghash, k);
  }

  return true;
}

int mlfs_hash_get_blocks(handle_t *handle, struct inode *inode,
			struct mlfs_map_blocks *map, int flags) {
	int err = 0;
	mlfs_lblk_t allocated = 0;
	int create;

	mlfs_assert(handle);

	create = flags & MLFS_GET_BLOCKS_CREATE_DATA;
  int ret = map->m_len;

  // lookup all blocks.
  uint32_t len = map->m_len;
  bool set = false;

  for (mlfs_lblk_t i = 0; i < map->m_len; ) {
    hash_value_t index;
    hash_value_t value;
    hash_value_t size;
    int pre = lookup_hash(inode, map->m_lblk + i, &value, &size, &index);
    if (!pre) {
      goto create;
    }

    if (!set) {
      //printf("Setting to %lu + %lu\n", value, index);
      map->m_pblk = value + index;
      set = true;
    }

    len -= size;
    i += size;

  }
  return ret;

create:
  if (create) {
    mlfs_fsblk_t blockp;
    struct super_block *sb = get_inode_sb(handle->dev, inode);
    enum alloc_type a_type;

    if (flags & MLFS_GET_BLOCKS_CREATE_DATA_LOG) {
      a_type = DATA_LOG;
    } else if (flags & MLFS_GET_BLOCKS_CREATE_META) {
      a_type = TREE;
    } else {
      a_type = DATA;
    }

    pthread_mutex_lock(&alloc_tex);
    mlfs_lblk_t lb = map->m_lblk + (map->m_len - len);
    int r = mlfs_new_blocks(sb, &blockp, len, 0, 0, a_type, 0);
    if (r > 0) {
      bitmap_bits_set_range(sb->s_blk_bitmap, blockp, r);
      sb->used_blocks += r;
    } else if (r == -ENOSPC) {
      panic("Failed to allocate block -- no space!\n");
    } else if (r == -EINVAL) {
      panic("Failed to allocate block -- invalid arguments!\n");
    } else {
      panic("Failed to allocate block -- unknown error!\n");
    }
    pthread_mutex_unlock(&alloc_tex);

    //printf("Starting insert: %u, %lu, %lu\n", map->m_lblk, map->m_len, len);
    for (int c = 0; c < len; ) {
      int offset = ((lb + c) & RANGE_BITS);
      int aligned = offset == 0;
      if (len >= (RANGE_SIZE / 2) && aligned) {
        /*
         * It's possible part of the range has already been allocated.
         * Say if someone requests (RANGE_SIZE + 1) blocks, but the blocks from
         * (RANGE_SIZE, RANGE_SIZE + RANGE_SIZE) have already been allocated,
         * we need to skip the last block.
         */
        hash_value_t index;
        hash_value_t value;
        hash_value_t size;
        int pre = lookup_hash(inode, lb + c, &value, &size, &index);
        if (pre) {
          c += size;
          if (!set) {
            map->m_pblk = value + index;
            set = true;
          }
          continue;
        }

        uint32_t nblocks = min(len - c, RANGE_SIZE);
        //printf("Insert to big.\n");
        hash_key_t k = RANGE_KEY(inode->inum, lb + c);
        int success = insert_hash(gsuper, inode, k, blockp, nblocks);
        if (!success) {
          panic("Could not insert huge range!\n");
        }

        if (!set) {
          map->m_pblk = blockp;
          set = true;
        }

        c += nblocks;
        blockp += nblocks;

      } else {
        uint32_t nblocks_to_alloc = min(len - c, RANGE_SIZE - offset);

        if (!set) {
          map->m_pblk = blockp;
          set = true;
        }

        //printf("Insert to small: %u.\n", nblocks_to_alloc);
        for (uint32_t i = 0; i < nblocks_to_alloc; ++i) {
          hash_key_t k = MAKEKEY(inode, lb + i + c);
          int success = insert_hash(ghash, inode, k, blockp,
              nblocks_to_alloc - i);

          if (!success) {
            fprintf(stderr, "could not insert: key = %u, val = %0lx\n",
                lb + i + c, blockp);
            panic("Could not insert into small table!");
          }

        }

        c += nblocks_to_alloc;
        blockp += nblocks_to_alloc;
      }
    }

    if (err) fprintf(stderr, "ERR = %d\n", err);

    map->m_pblk = blockp;

    mlfs_lblk_t lb = map->m_lblk + (map->m_len - len);
    for (uint32_t i = 0; i < len; ++i) {
      int success = insert_hash(inode, lb, blockp);

      if (!success) fprintf(stderr, "could not insert\n");

      blockp++;
      lb++;
    }
  }

  mlfs_hash_persist();
  return ret;
}

int mlfs_hash_truncate(handle_t *handle, struct inode *inode,
		mlfs_lblk_t start, mlfs_lblk_t end) {
  hash_value_t size, value, index;

  // TODO: probably inefficient
  for (mlfs_lblk_t i = start; i <= end;) {
    if (lookup_hash(inode, i, &value, &size, &index)) {
      mlfs_fsblk_t pblock = value + index;
      mlfs_free_blocks(handle, inode, NULL, pblock, size, 0);
      erase_hash(inode, i);
      i += size;
    }
  }

  return mlfs_hash_persist();
}

double check_load_factor(struct inode *inode) {
  double load = 0.0;
  double allocated_size = (double)ghash->size;
  double current_size = (double)ghash->noccupied;
  load = current_size / allocated_size;
  return load;
}

int mlfs_hash_persist() {
  pthread_mutex_lock(ghash->metalock);
  pthread_mutex_lock(gsuper->metalock);

  sync_all_buffers(g_bdev[g_root_dev]);
  nvram_flush(ghash);
  nvram_flush(gsuper);

  pthread_mutex_unlock(gsuper->metalock);
  pthread_mutex_unlock(ghash->metalock);

  return 0;
}

#endif
