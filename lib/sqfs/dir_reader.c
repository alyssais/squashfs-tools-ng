/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * fs_reader.c
 *
 * Copyright (C) 2019 David Oberhollenzer <goliath@infraroot.at>
 */
#define SQFS_BUILDING_DLL
#include "config.h"

#include "sqfs/meta_reader.h"
#include "sqfs/dir_reader.h"
#include "sqfs/compressor.h"
#include "sqfs/super.h"
#include "sqfs/inode.h"
#include "sqfs/error.h"
#include "sqfs/dir.h"
#include "util/util.h"

#include <string.h>
#include <stdlib.h>

struct sqfs_dir_reader_t {
	sqfs_meta_reader_t *meta_dir;
	sqfs_meta_reader_t *meta_inode;
	const sqfs_super_t *super;

	sqfs_dir_header_t hdr;
	sqfs_u64 dir_block_start;
	size_t entries;
	size_t size;

	size_t start_size;
	sqfs_u16 dir_offset;
	sqfs_u16 inode_offset;
};

sqfs_dir_reader_t *sqfs_dir_reader_create(const sqfs_super_t *super,
					  sqfs_compressor_t *cmp,
					  sqfs_file_t *file)
{
	sqfs_dir_reader_t *rd = calloc(1, sizeof(*rd));
	sqfs_u64 start, limit;

	if (rd == NULL)
		return NULL;

	start = super->inode_table_start;
	limit = super->directory_table_start;

	rd->meta_inode = sqfs_meta_reader_create(file, cmp, start, limit);

	if (rd->meta_inode == NULL) {
		free(rd);
		return NULL;
	}

	start = super->directory_table_start;
	limit = super->id_table_start;

	if (super->fragment_table_start < limit)
		limit = super->fragment_table_start;

	if (super->export_table_start < limit)
		limit = super->export_table_start;

	rd->meta_dir = sqfs_meta_reader_create(file, cmp, start, limit);

	if (rd->meta_dir == NULL) {
		sqfs_meta_reader_destroy(rd->meta_inode);
		free(rd);
		return NULL;
	}

	rd->super = super;
	return rd;
}

void sqfs_dir_reader_destroy(sqfs_dir_reader_t *rd)
{
	sqfs_meta_reader_destroy(rd->meta_inode);
	sqfs_meta_reader_destroy(rd->meta_dir);
	free(rd);
}

int sqfs_dir_reader_open_dir(sqfs_dir_reader_t *rd,
			     const sqfs_inode_generic_t *inode)
{
	sqfs_u64 block_start;
	size_t size, offset;

	if (inode->base.type == SQFS_INODE_DIR) {
		size = inode->data.dir.size;
		offset = inode->data.dir.offset;
		block_start = inode->data.dir.start_block;
	} else if (inode->base.type == SQFS_INODE_EXT_DIR) {
		size = inode->data.dir_ext.size;
		offset = inode->data.dir_ext.offset;
		block_start = inode->data.dir_ext.start_block;
	} else {
		return SQFS_ERROR_NOT_DIR;
	}

	memset(&rd->hdr, 0, sizeof(rd->hdr));
	rd->size = size;
	rd->entries = 0;

	if (rd->size <= sizeof(rd->hdr))
		return 0;

	block_start += rd->super->directory_table_start;

	rd->dir_block_start = block_start;
	rd->dir_offset = offset;
	rd->start_size = size;

	return sqfs_meta_reader_seek(rd->meta_dir, block_start, offset);
}

int sqfs_dir_reader_read(sqfs_dir_reader_t *rd, sqfs_dir_entry_t **out)
{
	sqfs_dir_entry_t *ent;
	size_t count;
	int err;

	if (!rd->entries) {
		if (rd->size < sizeof(rd->hdr))
			return 1;

		err = sqfs_meta_reader_read_dir_header(rd->meta_dir, &rd->hdr);
		if (err)
			return err;

		rd->size -= sizeof(rd->hdr);
		rd->entries = rd->hdr.count + 1;
	}

	err = sqfs_meta_reader_read_dir_ent(rd->meta_dir, &ent);
	if (err)
		return err;

	count = sizeof(*ent) + strlen((const char *)ent->name);

	if (count > rd->size) {
		rd->size = 0;
		rd->entries = 0;
	} else {
		rd->size -= count;
		rd->entries -= 1;
	}

	rd->inode_offset = ent->offset;
	*out = ent;
	return 0;
}

int sqfs_dir_reader_rewind(sqfs_dir_reader_t *rd)
{
	memset(&rd->hdr, 0, sizeof(rd->hdr));
	rd->size = rd->start_size;
	rd->entries = 0;

	return sqfs_meta_reader_seek(rd->meta_dir, rd->dir_block_start,
				     rd->dir_offset);
}

int sqfs_dir_reader_find(sqfs_dir_reader_t *rd, const char *name)
{
	sqfs_dir_entry_t *ent;
	int ret;

	if (rd->size != rd->start_size) {
		ret = sqfs_dir_reader_rewind(rd);
		if (ret)
			return ret;
	}

	do {
		ret = sqfs_dir_reader_read(rd, &ent);
		if (ret < 0)
			return ret;
		if (ret > 0)
			return SQFS_ERROR_NO_ENTRY;

		ret = strcmp((const char *)ent->name, name);
		free(ent);
	} while (ret < 0);

	return ret == 0 ? 0 : SQFS_ERROR_NO_ENTRY;
}

int sqfs_dir_reader_get_inode(sqfs_dir_reader_t *rd,
			      sqfs_inode_generic_t **inode)
{
	sqfs_u64 block_start;

	block_start = rd->hdr.start_block;

	return sqfs_meta_reader_read_inode(rd->meta_inode, rd->super,
					   block_start, rd->inode_offset,
					   inode);
}

int sqfs_dir_reader_get_root_inode(sqfs_dir_reader_t *rd,
				   sqfs_inode_generic_t **inode)
{
	sqfs_u64 block_start = rd->super->root_inode_ref >> 16;
	sqfs_u16 offset = rd->super->root_inode_ref & 0xFFFF;

	return sqfs_meta_reader_read_inode(rd->meta_inode, rd->super,
					   block_start, offset, inode);
}

int sqfs_dir_reader_find_by_path(sqfs_dir_reader_t *rd, const char *path,
				 sqfs_inode_generic_t **out)
{
	sqfs_inode_generic_t *inode;
	sqfs_dir_entry_t *ent;
	const char *ptr;
	int ret;

	ret = sqfs_dir_reader_get_root_inode(rd, &inode);
	if (ret)
		return ret;

	while (*path != '\0') {
		if (*path == '/' || *path == '\\') {
			while (*path == '/' || *path == '\\')
				++path;
			continue;
		}

		ret = sqfs_dir_reader_open_dir(rd, inode);
		free(inode);
		if (ret)
			return ret;

		ptr = strchr(path, '/');
		if (ptr == NULL) {
			ptr = strchr(path, '\\');

			if (ptr == NULL) {
				for (ptr = path; *ptr != '\0'; ++ptr)
					;
			}
		}

		do {
			ret = sqfs_dir_reader_read(rd, &ent);
			if (ret < 0)
				return ret;

			if (ret == 0) {
				ret = strncmp((const char *)ent->name,
					      path, ptr - path);
				if (ret == 0)
					ret = ent->name[ptr - path];
				free(ent);
			}
		} while (ret < 0);

		if (ret > 0)
			return SQFS_ERROR_NO_ENTRY;

		ret = sqfs_dir_reader_get_inode(rd, &inode);
		if (ret)
			return ret;

		path = ptr;
	}

	*out = inode;
	return 0;
}
