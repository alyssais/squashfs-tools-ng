/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * zstd.c
 *
 * Copyright (C) 2019 David Oberhollenzer <goliath@infraroot.at>
 */
#define SQFS_BUILDING_DLL
#include "config.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <zstd.h>

#include "internal.h"

typedef struct {
	sqfs_compressor_t base;
	ZSTD_CCtx *zctx;
	int level;
} zstd_compressor_t;

typedef struct {
	sqfs_u32 level;
} zstd_options_t;

static int zstd_write_options(sqfs_compressor_t *base, sqfs_file_t *file)
{
	zstd_compressor_t *zstd = (zstd_compressor_t *)base;
	zstd_options_t opt;

	if (zstd->level == SQFS_ZSTD_DEFAULT_LEVEL)
		return 0;

	opt.level = htole32(zstd->level);
	return sqfs_generic_write_options(file, &opt, sizeof(opt));
}

static int zstd_read_options(sqfs_compressor_t *base, sqfs_file_t *file)
{
	zstd_options_t opt;
	int ret;
	(void)base;

	ret = sqfs_generic_read_options(file, &opt, sizeof(opt));
	if (ret)
		return ret;

	opt.level = le32toh(opt.level);
	return 0;
}

static sqfs_s32 zstd_comp_block(sqfs_compressor_t *base, const sqfs_u8 *in,
				sqfs_u32 size, sqfs_u8 *out, sqfs_u32 outsize)
{
	zstd_compressor_t *zstd = (zstd_compressor_t *)base;
	size_t ret;

	if (size >= 0x7FFFFFFF)
		return 0;

	ret = ZSTD_compressCCtx(zstd->zctx, out, outsize, in, size,
				zstd->level);

	if (ZSTD_isError(ret))
		return SQFS_ERROR_COMPRESSOR;

	return ret < size ? ret : 0;
}

static sqfs_s32 zstd_uncomp_block(sqfs_compressor_t *base, const sqfs_u8 *in,
				  sqfs_u32 size, sqfs_u8 *out, sqfs_u32 outsize)
{
	size_t ret;
	(void)base;

	if (outsize >= 0x7FFFFFFF)
		return 0;

	ret = ZSTD_decompress(out, outsize, in, size);

	if (ZSTD_isError(ret))
		return SQFS_ERROR_COMPRESSOR;

	return ret;
}

static sqfs_compressor_t *zstd_create_copy(sqfs_compressor_t *cmp)
{
	zstd_compressor_t *zstd = malloc(sizeof(*zstd));

	if (zstd == NULL)
		return NULL;

	memcpy(zstd, cmp, sizeof(*zstd));

	zstd->zctx = ZSTD_createCCtx();

	if (zstd->zctx == NULL) {
		free(zstd);
		return NULL;
	}

	return (sqfs_compressor_t *)zstd;
}

static void zstd_destroy(sqfs_compressor_t *base)
{
	zstd_compressor_t *zstd = (zstd_compressor_t *)base;

	ZSTD_freeCCtx(zstd->zctx);
	free(zstd);
}

sqfs_compressor_t *zstd_compressor_create(const sqfs_compressor_config_t *cfg)
{
	zstd_compressor_t *zstd;
	sqfs_compressor_t *base;

	if (cfg->flags & ~SQFS_COMP_FLAG_GENERIC_ALL)
		return NULL;

	if (cfg->opt.zstd.level < 1 ||
	    cfg->opt.zstd.level > ZSTD_maxCLevel()) {
		return NULL;
	}

	zstd = calloc(1, sizeof(*zstd));
	base = (sqfs_compressor_t *)zstd;
	if (zstd == NULL)
		return NULL;

	zstd->zctx = ZSTD_createCCtx();
	if (zstd->zctx == NULL) {
		free(zstd);
		return NULL;
	}

	base->destroy = zstd_destroy;
	base->do_block = cfg->flags & SQFS_COMP_FLAG_UNCOMPRESS ?
		zstd_uncomp_block : zstd_comp_block;
	base->write_options = zstd_write_options;
	base->read_options = zstd_read_options;
	base->create_copy = zstd_create_copy;
	return base;
}
