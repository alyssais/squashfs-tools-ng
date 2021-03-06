/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * process_block.c
 *
 * Copyright (C) 2019 David Oberhollenzer <goliath@infraroot.at>
 */
#define SQFS_BUILDING_DLL
#include "internal.h"

#include <string.h>

static int store_block_location(sqfs_data_writer_t *proc, sqfs_u64 offset,
				sqfs_u32 size, sqfs_u32 chksum)
{
	size_t new_sz;
	void *new;

	if (proc->num_blocks == proc->max_blocks) {
		new_sz = proc->max_blocks * 2;
		new = realloc(proc->blocks, sizeof(proc->blocks[0]) * new_sz);

		if (new == NULL)
			return SQFS_ERROR_ALLOC;

		proc->blocks = new;
		proc->max_blocks = new_sz;
	}

	proc->blocks[proc->num_blocks].offset = offset;
	proc->blocks[proc->num_blocks].hash = MK_BLK_HASH(chksum, size);
	proc->num_blocks += 1;
	return 0;
}

static size_t deduplicate_blocks(sqfs_data_writer_t *proc, size_t count)
{
	size_t i, j;

	for (i = 0; i < proc->file_start; ++i) {
		for (j = 0; j < count; ++j) {
			if (proc->blocks[i + j].hash !=
			    proc->blocks[proc->file_start + j].hash)
				break;
		}

		if (j == count)
			break;
	}

	return i;
}

static int align_file(sqfs_data_writer_t *proc, sqfs_block_t *blk)
{
	sqfs_u32 chksum;
	void *padding;
	sqfs_u64 size;
	size_t diff;
	int ret;

	if (!(blk->flags & SQFS_BLK_ALIGN))
		return 0;

	size = proc->file->get_size(proc->file);
	diff = size % proc->devblksz;
	if (diff == 0)
		return 0;

	padding = calloc(1, diff);
	if (padding == 0)
		return SQFS_ERROR_ALLOC;

	if (proc->hooks != NULL && proc->hooks->prepare_padding != NULL)
		proc->hooks->prepare_padding(proc->user_ptr, padding, diff);

	chksum = crc32(0, padding, diff);

	ret = proc->file->write_at(proc->file, size, padding, diff);
	free(padding);
	if (ret)
		return ret;

	return store_block_location(proc, size, diff | (1 << 24), chksum);
}

int process_completed_block(sqfs_data_writer_t *proc, sqfs_block_t *blk)
{
	sqfs_u64 offset, bytes;
	size_t start, count;
	sqfs_u32 out;
	int err;

	if (proc->hooks != NULL && proc->hooks->pre_block_write != NULL) {
		proc->hooks->pre_block_write(proc->user_ptr, blk, proc->file);
	}

	if (blk->flags & SQFS_BLK_FIRST_BLOCK) {
		proc->start = proc->file->get_size(proc->file);
		proc->file_start = proc->num_blocks;

		err = align_file(proc, blk);
		if (err)
			return err;
	}

	if (blk->size != 0) {
		out = blk->size;
		if (!(blk->flags & SQFS_BLK_IS_COMPRESSED))
			out |= 1 << 24;

		offset = proc->file->get_size(proc->file);

		if (blk->flags & SQFS_BLK_FRAGMENT_BLOCK) {
			offset = htole64(offset);
			proc->fragments[blk->index].start_offset = offset;
			proc->fragments[blk->index].pad0 = 0;
			proc->fragments[blk->index].size = htole32(out);
		} else {
			blk->inode->block_sizes[blk->index] = out;
		}

		err = store_block_location(proc, offset, out, blk->checksum);
		if (err)
			return err;

		err = proc->file->write_at(proc->file, offset,
					   blk->data, blk->size);
		if (err)
			return err;
	}

	if (proc->hooks != NULL && proc->hooks->post_block_write != NULL) {
		proc->hooks->post_block_write(proc->user_ptr, blk, proc->file);
	}

	if (blk->flags & SQFS_BLK_LAST_BLOCK) {
		err = align_file(proc, blk);
		if (err)
			return err;

		count = proc->num_blocks - proc->file_start;
		start = deduplicate_blocks(proc, count);
		offset = proc->blocks[start].offset;

		sqfs_inode_set_file_block_start(blk->inode, offset);

		if (start >= proc->file_start)
			return 0;

		offset = start + count;
		if (offset >= proc->file_start) {
			count = proc->num_blocks - offset;
			proc->num_blocks = offset;
		} else {
			proc->num_blocks = proc->file_start;
		}

		if (proc->hooks != NULL &&
		    proc->hooks->notify_blocks_erased != NULL) {
			bytes = proc->file->get_size(proc->file) - proc->start;

			proc->hooks->notify_blocks_erased(proc->user_ptr,
							  count, bytes);
		}

		err = proc->file->truncate(proc->file, proc->start);
		if (err)
			return err;
	}

	return 0;
}
