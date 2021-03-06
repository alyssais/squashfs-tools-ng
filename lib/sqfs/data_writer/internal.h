/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * internal.h
 *
 * Copyright (C) 2019 David Oberhollenzer <goliath@infraroot.at>
 */
#ifndef INTERNAL_H
#define INTERNAL_H

#include "config.h"

#include "sqfs/data_writer.h"
#include "sqfs/compressor.h"
#include "sqfs/inode.h"
#include "sqfs/table.h"
#include "sqfs/error.h"
#include "sqfs/block.h"
#include "sqfs/io.h"
#include "util/util.h"

#include <string.h>
#include <stdlib.h>
#include <zlib.h>

#ifdef WITH_PTHREAD
#include <pthread.h>
#endif


#define MK_BLK_HASH(chksum, size) \
	(((sqfs_u64)(size) << 32) | (sqfs_u64)(chksum))

#define INIT_BLOCK_COUNT (128)


typedef struct {
	sqfs_u64 offset;
	sqfs_u64 hash;
} blk_info_t;

typedef struct {
	sqfs_u32 index;
	sqfs_u32 offset;
	sqfs_u64 hash;
} frag_info_t;


#ifdef WITH_PTHREAD
typedef struct {
	sqfs_data_writer_t *shared;
	sqfs_compressor_t *cmp;
	pthread_t thread;
	sqfs_u8 scratch[];
} compress_worker_t;
#endif

struct sqfs_data_writer_t {
	/* synchronization primitives */
#ifdef WITH_PTHREAD
	pthread_mutex_t mtx;
	pthread_cond_t queue_cond;
	pthread_cond_t done_cond;
#endif

	/* needs rw access by worker and main thread */
	sqfs_block_t *queue;
	sqfs_block_t *queue_last;

	sqfs_block_t *done;
	size_t backlog;
	int status;

	/* used by main thread only */
	sqfs_u32 enqueue_id;
	sqfs_u32 dequeue_id;

	unsigned int num_workers;
	size_t max_backlog;

	size_t devblksz;
	sqfs_file_t *file;

	sqfs_fragment_t *fragments;
	size_t num_fragments;
	size_t max_fragments;

	sqfs_u64 start;

	size_t file_start;
	size_t num_blocks;
	size_t max_blocks;
	blk_info_t *blocks;
	sqfs_compressor_t *cmp;

	sqfs_block_t *frag_block;
	frag_info_t *frag_list;
	size_t frag_list_num;
	size_t frag_list_max;

	const sqfs_block_hooks_t *hooks;
	void *user_ptr;

	/* file API */
	sqfs_inode_generic_t *inode;
	sqfs_block_t *blk_current;
	sqfs_u32 blk_flags;
	size_t blk_index;

	/* used only by workers */
	size_t max_block_size;

#ifdef WITH_PTHREAD
	compress_worker_t *workers[];
#else
	sqfs_u8 scratch[];
#endif
};

SQFS_INTERNAL int process_completed_block(sqfs_data_writer_t *proc,
					  sqfs_block_t *block);

SQFS_INTERNAL
int process_completed_fragment(sqfs_data_writer_t *proc, sqfs_block_t *frag,
			       sqfs_block_t **blk_out);

SQFS_INTERNAL void free_blk_list(sqfs_block_t *list);

SQFS_INTERNAL
int data_writer_init(sqfs_data_writer_t *proc, size_t max_block_size,
		     sqfs_compressor_t *cmp, unsigned int num_workers,
		     size_t max_backlog, size_t devblksz, sqfs_file_t *file);

SQFS_INTERNAL void data_writer_cleanup(sqfs_data_writer_t *proc);

SQFS_INTERNAL
void data_writer_store_done(sqfs_data_writer_t *proc, sqfs_block_t *blk,
			    int status);

SQFS_INTERNAL
sqfs_block_t *data_writer_next_work_item(sqfs_data_writer_t *proc);

SQFS_INTERNAL
int data_writer_do_block(sqfs_block_t *block, sqfs_compressor_t *cmp,
			 sqfs_u8 *scratch, size_t scratch_size);

SQFS_INTERNAL
int test_and_set_status(sqfs_data_writer_t *proc, int status);

SQFS_INTERNAL
int data_writer_enqueue(sqfs_data_writer_t *proc, sqfs_block_t *block);

#endif /* INTERNAL_H */
