#include "sqfs/compressor.h"
#include "sqfs/dir_reader.h"
#include "sqfs/id_table.h"
#include "sqfs/inode.h"
#include "sqfs/super.h"
#include "sqfs/io.h"

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>

static void write_tree_dfs(const sqfs_tree_node_t *n)
{
	const sqfs_tree_node_t *p;
	unsigned int mask, level;
	int i;

	for (n = n->children; n != NULL; n = n->next) {
		level = 0;
		mask = 0;

		for (p = n->parent; p->parent != NULL; p = p->parent) {
			if (p->next != NULL)
				mask |= 1 << level;
			++level;
		}

		for (i = level - 1; i >= 0; --i)
			fputs(mask & (1 << i) ? "│  " : "   ", stdout);

		fputs(n->next == NULL ? "└─ " : "├─ ", stdout);
		fputs((const char *)n->name, stdout);

		if (n->inode->base.type == SQFS_INODE_SLINK) {
			printf(" ⭢ %.*s", n->inode->data.slink.target_size,
			       n->inode->slink_target);
		} else if (n->inode->base.type == SQFS_INODE_EXT_SLINK) {
			printf(" ⭢ %.*s", n->inode->data.slink_ext.target_size,
			       n->inode->slink_target);
		}

		fputc('\n', stdout);
		write_tree_dfs(n);
	}
}

int main(int argc, char **argv)
{
	sqfs_compressor_config_t cfg;
	sqfs_compressor_t *cmp;
	sqfs_tree_node_t *root = NULL;
	sqfs_id_table_t *idtbl;
	sqfs_dir_reader_t *dr;
	sqfs_file_t *file;
	sqfs_super_t super;
	int status = EXIT_FAILURE;

	/* open the SquashFS file we want to read */
	if (argc != 2) {
		fputs("Usage: list_files <squashfs-file>\n", stderr);
		return EXIT_FAILURE;
	}

	file = sqfs_open_file(argv[1], SQFS_FILE_OPEN_READ_ONLY);
	if (file == NULL) {
		perror(argv[1]);
		return EXIT_FAILURE;
	}

	/* read the super block, create a compressor and
	   process the compressor options */
	if (sqfs_super_read(&super, file)) {
		fprintf(stderr, "%s: error reading super block.\n", argv[1]);
		goto out_fd;
	}

	if (!sqfs_compressor_exists(super.compression_id)) {
		fprintf(stderr, "%s: unknown compressor used.\n", argv[1]);
		goto out_fd;
	}

	sqfs_compressor_config_init(&cfg, super.compression_id,
				    super.block_size,
				    SQFS_COMP_FLAG_UNCOMPRESS);

	cmp = sqfs_compressor_create(&cfg);
	if (cmp == NULL) {
		fprintf(stderr, "%s: error creating compressor.\n", argv[1]);
		goto out_fd;
	}

	if (super.flags & SQFS_FLAG_COMPRESSOR_OPTIONS) {
		if (cmp->read_options(cmp, file)) {
			fprintf(stderr,
				"%s: error reading compressor options.\n",
				argv[1]);
			goto out_cmp;
		}
	}

	/* Create and read the UID/GID mapping table */
	idtbl = sqfs_id_table_create();
	if (idtbl == NULL) {
		fputs("Error creating ID table.\n", stderr);
		goto out_cmp;
	}

	if (sqfs_id_table_read(idtbl, file, &super, cmp)) {
		fprintf(stderr, "%s: error loading ID table.\n", argv[1]);
		goto out_id;
	}

	/* create a directory reader and scan the entire directory hiearchy */
	dr = sqfs_dir_reader_create(&super, cmp, file);
	if (dr == NULL) {
		fprintf(stderr, "%s: error creating directory reader.\n",
			argv[1]);
		goto out_id;
	}

	if (sqfs_dir_reader_get_full_hierarchy(dr, idtbl, NULL, 0, &root)) {
		fprintf(stderr, "%s: error loading directory tree.\n",
			argv[1]);
		goto out;
	}

	/* fancy print the hierarchy */
	printf("/\n");
	write_tree_dfs(root);

	/* cleanup */
	status = EXIT_SUCCESS;
out:
	if (root != NULL)
		sqfs_dir_tree_destroy(root);
	sqfs_dir_reader_destroy(dr);
out_id:
	sqfs_id_table_destroy(idtbl);
out_cmp:
	cmp->destroy(cmp);
out_fd:
	file->destroy(file);
	return status;
}
