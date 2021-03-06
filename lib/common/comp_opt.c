/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * comp_opt.c
 *
 * Copyright (C) 2019 David Oberhollenzer <goliath@infraroot.at>
 */
#include "common.h"

#include <string.h>
#include <stdlib.h>
#include <getopt.h>
#include <ctype.h>

typedef struct {
	const char *name;
	sqfs_u16 flag;
} flag_t;

static const flag_t gzip_flags[] = {
	{ "default", SQFS_COMP_FLAG_GZIP_DEFAULT },
	{ "filtered", SQFS_COMP_FLAG_GZIP_FILTERED },
	{ "huffman", SQFS_COMP_FLAG_GZIP_HUFFMAN },
	{ "rle", SQFS_COMP_FLAG_GZIP_RLE },
	{ "fixed", SQFS_COMP_FLAG_GZIP_FIXED },
};

static const flag_t xz_flags[] = {
	{ "x86", SQFS_COMP_FLAG_XZ_X86 },
	{ "powerpc", SQFS_COMP_FLAG_XZ_POWERPC },
	{ "ia64", SQFS_COMP_FLAG_XZ_IA64 },
	{ "arm", SQFS_COMP_FLAG_XZ_ARM },
	{ "armthumb", SQFS_COMP_FLAG_XZ_ARMTHUMB },
	{ "sparc", SQFS_COMP_FLAG_XZ_SPARC },
};

static const flag_t lz4_flags[] = {
	{ "hc", SQFS_COMP_FLAG_LZ4_HC },
};

static const char *lzo_algs[] = {
	[SQFS_LZO1X_1] = "lzo1x_1",
	[SQFS_LZO1X_1_11] = "lzo1x_1_11",
	[SQFS_LZO1X_1_12] = "lzo1x_1_12",
	[SQFS_LZO1X_1_15] = "lzo1x_1_15",
	[SQFS_LZO1X_999] = "lzo1x_999",
};

static int set_flag(sqfs_compressor_config_t *cfg, const char *name,
		    const flag_t *flags, size_t num_flags)
{
	size_t i;

	for (i = 0; i < num_flags; ++i) {
		if (strcmp(flags[i].name, name) == 0) {
			cfg->flags |= flags[i].flag;
			return 0;
		}
	}

	return -1;
}

static int find_lzo_alg(sqfs_compressor_config_t *cfg, const char *name)
{
	size_t i;

	for (i = 0; i < sizeof(lzo_algs) / sizeof(lzo_algs[0]); ++i) {
		if (strcmp(lzo_algs[i], name) == 0) {
			cfg->opt.lzo.algorithm = i;
			return 0;
		}
	}

	return -1;
}

static int get_size_value(const char *value, sqfs_u32 *out, sqfs_u32 block_size)
{
	int i;

	for (i = 0; isdigit(value[i]) && i < 9; ++i)
		;

	if (i < 1 || i > 9)
		return -1;

	*out = atol(value);

	switch (value[i]) {
	case '\0':
		break;
	case 'm':
	case 'M':
		*out <<= 20;
		break;
	case 'k':
	case 'K':
		*out <<= 10;
		break;
	case '%':
		*out = (*out * block_size) / 100;
		break;
	default:
		return -1;
	}

	return 0;
}

enum {
	OPT_WINDOW = 0,
	OPT_LEVEL,
	OPT_ALG,
	OPT_DICT,
};
static char *const token[] = {
	[OPT_WINDOW] = (char *)"window",
	[OPT_LEVEL] = (char *)"level",
	[OPT_ALG] = (char *)"algorithm",
	[OPT_DICT] = (char *)"dictsize",
	NULL
};

int compressor_cfg_init_options(sqfs_compressor_config_t *cfg,
				E_SQFS_COMPRESSOR id,
				size_t block_size, char *options)
{
	size_t num_flags = 0, min_level = 0, max_level = 0, level;
	const flag_t *flags = NULL;
	char *subopts, *value;
	int i, opt;

	if (sqfs_compressor_config_init(cfg, id, block_size, 0))
		return -1;

	if (options == NULL)
		return 0;

	switch (cfg->id) {
	case SQFS_COMP_GZIP:
		min_level = SQFS_GZIP_MIN_LEVEL;
		max_level = SQFS_GZIP_MAX_LEVEL;
		flags = gzip_flags;
		num_flags = sizeof(gzip_flags) / sizeof(gzip_flags[0]);
		break;
	case SQFS_COMP_LZO:
		min_level = SQFS_LZO_MIN_LEVEL;
		max_level = SQFS_LZO_MAX_LEVEL;
		break;
	case SQFS_COMP_ZSTD:
		min_level = SQFS_ZSTD_MIN_LEVEL;
		max_level = SQFS_ZSTD_MAX_LEVEL;
		break;
	case SQFS_COMP_XZ:
		flags = xz_flags;
		num_flags = sizeof(xz_flags) / sizeof(xz_flags[0]);
		break;
	case SQFS_COMP_LZ4:
		flags = lz4_flags;
		num_flags = sizeof(lz4_flags) / sizeof(lz4_flags[0]);
		break;
	default:
		break;
	}

	subopts = options;

	while (*subopts != '\0') {
		opt = getsubopt(&subopts, token, &value);

		switch (opt) {
		case OPT_WINDOW:
			if (cfg->id != SQFS_COMP_GZIP)
				goto fail_opt;

			if (value == NULL)
				goto fail_value;

			for (i = 0; isdigit(value[i]); ++i)
				;

			if (i < 1 || i > 3 || value[i] != '\0')
				goto fail_window;

			cfg->opt.gzip.window_size = atoi(value);

			if (cfg->opt.gzip.window_size < SQFS_GZIP_MIN_WINDOW ||
			    cfg->opt.gzip.window_size > SQFS_GZIP_MAX_WINDOW)
				goto fail_window;
			break;
		case OPT_LEVEL:
			if (value == NULL)
				goto fail_value;

			for (i = 0; isdigit(value[i]) && i < 3; ++i)
				;

			if (i < 1 || i > 3 || value[i] != '\0')
				goto fail_level;

			level = atoi(value);

			if (level < min_level || level > max_level)
				goto fail_level;

			switch (cfg->id) {
			case SQFS_COMP_GZIP:
				cfg->opt.gzip.level = level;
				break;
			case SQFS_COMP_LZO:
				cfg->opt.lzo.level = level;
				break;
			case SQFS_COMP_ZSTD:
				cfg->opt.zstd.level = level;
				break;
			default:
				goto fail_opt;
			}
			break;
		case OPT_ALG:
			if (cfg->id != SQFS_COMP_LZO)
				goto fail_opt;

			if (value == NULL)
				goto fail_value;

			if (find_lzo_alg(cfg, value))
				goto fail_lzo_alg;
			break;
		case OPT_DICT:
			if (cfg->id != SQFS_COMP_XZ)
				goto fail_opt;

			if (value == NULL)
				goto fail_value;

			if (get_size_value(value, &cfg->opt.xz.dict_size,
					   cfg->block_size)) {
				goto fail_dict;
			}
			break;
		default:
			if (set_flag(cfg, value, flags, num_flags))
				goto fail_opt;
			break;
		}
	}

	return 0;
fail_lzo_alg:
	fprintf(stderr, "Unknown lzo variant '%s'.\n", value);
	return -1;
fail_window:
	fputs("Window size must be a number between 8 and 15.\n", stderr);
	return -1;
fail_level:
	fprintf(stderr,
		"Compression level must be a number between %zu and %zu.\n",
		min_level, max_level);
	return -1;
fail_opt:
	fprintf(stderr, "Unknown compressor option '%s'.\n", value);
	return -1;
fail_value:
	fprintf(stderr, "Missing value for compressor option '%s'.\n",
		token[opt]);
	return -1;
fail_dict:
	fputs("Dictionary size must be a number with the optional "
	      "suffix 'm','k' or '%'.\n", stderr);
	return -1;
}

typedef void (*compressor_help_fun_t)(void);

static void gzip_print_help(void)
{
	size_t i;

	printf(
"Available options for gzip compressor:\n"
"\n"
"    level=<value>    Compression level. Value from 1 to 9.\n"
"                     Defaults to %d.\n"
"    window=<size>    Deflate compression window size. Value from 8 to 15.\n"
"                     Defaults to %d.\n"
"\n"
"In additon to the options, one or more strategies can be specified.\n"
"If multiple stratgies are provided, the one yielding the best compression\n"
"ratio will be used.\n"
"\n"
"The following strategies are available:\n",
	SQFS_GZIP_DEFAULT_LEVEL, SQFS_GZIP_DEFAULT_WINDOW);

	for (i = 0; i < sizeof(gzip_flags) / sizeof(gzip_flags[0]); ++i)
		printf("\t%s\n", gzip_flags[i].name);
}

static void lz4_print_help(void)
{
	fputs("Available options for lz4 compressor:\n"
	      "\n"
	      "    hc    If present, use slower but better compressing\n"
	      "          variant of lz4.\n"
	      "\n",
	      stdout);
}

static void lzo_print_help(void)
{
	size_t i;

	fputs("Available options for lzo compressor:\n"
	      "\n"
	      "    algorithm=<name>  Specify the variant of lzo to use.\n"
	      "                      Defaults to 'lzo1x_999'.\n"
	      "    level=<value>     For lzo1x_999, the compression level.\n"
	      "                      Value from 1 to 9. Defaults to 8.\n"
	      "                      Ignored if algorithm is not lzo1x_999.\n"
	      "\n"
	      "Available algorithms:\n",
	      stdout);

	for (i = 0; i < sizeof(lzo_algs) / sizeof(lzo_algs[0]); ++i)
		printf("\t%s\n", lzo_algs[i]);
}

static void xz_print_help(void)
{
	size_t i;

	fputs(
"Available options for xz compressor:\n"
"\n"
"    dictsize=<value>  Dictionary size. Either a value in bytes or a\n"
"                      percentage of the block size. Defaults to 100%.\n"
"                      The suffix '%' indicates a percentage. 'K' and 'M'\n"
"                      can also be used for kibi and mebi bytes\n"
"                      respecitively.\n"
"\n"
"In additon to the options, one or more bcj filters can be specified.\n"
"If multiple filters are provided, the one yielding the best compression\n"
"ratio will be used.\n"
"\n"
"The following filters are available:\n",
	stdout);

	for (i = 0; i < sizeof(xz_flags) / sizeof(xz_flags[0]); ++i)
		printf("\t%s\n", xz_flags[i].name);
}

static void zstd_print_help(void)
{
	printf("Available options for zstd compressor:\n"
	       "\n"
	       "    level=<value>    Set compression level. Defaults to %d.\n"
	       "                     Maximum is %d.\n"
	       "\n",
	       SQFS_ZSTD_DEFAULT_LEVEL, SQFS_ZSTD_MAX_LEVEL);
}

static const compressor_help_fun_t helpfuns[SQFS_COMP_MAX + 1] = {
	[SQFS_COMP_GZIP] = gzip_print_help,
	[SQFS_COMP_XZ] = xz_print_help,
	[SQFS_COMP_LZO] = lzo_print_help,
	[SQFS_COMP_LZ4] = lz4_print_help,
	[SQFS_COMP_ZSTD] = zstd_print_help,
};

void compressor_print_help(E_SQFS_COMPRESSOR id)
{
	if (id < SQFS_COMP_MIN || id > SQFS_COMP_MAX)
		return;

	if (helpfuns[id] == NULL)
		return;

	helpfuns[id]();
}
