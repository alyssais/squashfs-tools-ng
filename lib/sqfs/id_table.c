/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "id_table.h"
#include "table.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

int id_table_init(id_table_t *tbl)
{
	memset(tbl, 0, sizeof(*tbl));
	return 0;
}

void id_table_cleanup(id_table_t *tbl)
{
	free(tbl->ids);
}

int id_table_id_to_index(id_table_t *tbl, uint32_t id, uint16_t *out)
{
	size_t i, sz;
	void *ptr;

	for (i = 0; i < tbl->num_ids; ++i) {
		if (tbl->ids[i] == id) {
			*out = i;
			return 0;
		}
	}

	if (tbl->num_ids == 0x10000) {
		fputs("Too many unique UIDs/GIDs (more than 64k)!\n", stderr);
		return -1;
	}

	if (tbl->num_ids == tbl->max_ids) {
		sz = (tbl->max_ids ? tbl->max_ids * 2 : 16);
		ptr = realloc(tbl->ids, sizeof(tbl->ids[0]) * sz);

		if (ptr == NULL) {
			perror("growing ID table");
			return -1;
		}

		tbl->ids = ptr;
		tbl->max_ids = sz;
	}

	*out = tbl->num_ids;
	tbl->ids[tbl->num_ids++] = id;
	return 0;
}

int id_table_write(id_table_t *tbl, int outfd, sqfs_super_t *super,
		   compressor_t *cmp)
{
	size_t i;
	int ret;

	for (i = 0; i < tbl->num_ids; ++i)
		tbl->ids[i] = htole32(tbl->ids[i]);

	super->id_count = tbl->num_ids;

	ret = sqfs_write_table(outfd, super, tbl->ids, sizeof(tbl->ids[0]),
			       tbl->num_ids, &super->id_table_start, cmp);

	for (i = 0; i < tbl->num_ids; ++i)
		tbl->ids[i] = le32toh(tbl->ids[i]);

	return ret;
}