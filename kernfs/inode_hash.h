#ifndef __INODE_HASH__
#define __INODE_HASH__

#include <malloc.h>
#include <memory.h>
#include <string.h>
#include "fs.h"
#include "extents.h"
#include "global/util.h"
#ifdef KERNFS
#include "balloc.h"
#include "migrate.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define CONTINUITY_BITS 4
#define MAX_CONTIGUOUS_BLOCKS (2 << 4)
#define REMAINING_BITS ((CHAR_BIT * sizeof(mlfs_fsblk_t)) - CONTINUITY_BITS - 1)

#define TO_64(hk) (*((uint64_t*)&hk))

typedef mlfs_fsblk_t hash_value_t;

#define SPECIAL(hv) !(!(hv & (1UL << 63UL)))
#define INDEX(hv) ((hv >> REMAINING_BITS) & ((1lu << CONTINUITY_BITS) - 1lu))
#define ADDR(hv) (((1lu << REMAINING_BITS) - 1lu) & hv)

#define SBIT(cond) (cond ? (1UL << 63UL) : 0UL)
#define IBITS(i) ((uint64_t)i << REMAINING_BITS)

#define MAKEVAL(cond, idx, addr) (SBIT(cond) | IBITS(idx) | addr)

#define GVAL2PTR(hv) ((void*)(hv))
#define GPTR2VAL(pt) ((uint64_t)pt)


#define MASK_32 ((1lu << 32) - 1)
#define MAKEKEY(inode, key) (((uint64_t)inode->inum << 32) | key)
#define GET_INUM(hk) (hk >> 32)
#define GET_LBLK(hk) (hk & MASK_32)
#define GKEY2PTR(hk) ((void*)(hk))
#define GPTR2KEY(pt) ((uint64_t)pt)
typedef uint64_t hash_key_t;

/*
 * Generic hash table functions.
 */

void init_hash(struct inode *inode);

int insert_hash(struct inode *inode, mlfs_lblk_t key, hash_value_t value);

int lookup_hash(struct inode *inode, mlfs_lblk_t key, hash_value_t* value);

/*
 * Emulated mlfs_ext functions.
 */

int mlfs_hash_get_blocks(handle_t *handle, struct inode *inode,
			struct mlfs_map_blocks *map, int flags);

int mlfs_hash_truncate(handle_t *handle, struct inode *inode,
		mlfs_lblk_t start, mlfs_lblk_t end);

#endif  // __INODE_HASH__
