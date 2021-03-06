/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * mkfs.c
 *
 * Copyright (C) 2019 David Oberhollenzer <goliath@infraroot.at>
 */
#include "mkfs.h"

static int set_working_dir(options_t *opt)
{
	const char *ptr;

	if (opt->packdir != NULL)
		return pushd(opt->packdir);

	ptr = strrchr(opt->infile, '/');
	if (ptr != NULL)
		return pushdn(opt->infile, ptr - opt->infile);

	return 0;
}

static int restore_working_dir(options_t *opt)
{
	if (opt->packdir != NULL || strrchr(opt->infile, '/') != NULL)
		return popd();

	return 0;
}

static int pack_files(sqfs_data_writer_t *data, fstree_t *fs,
		      data_writer_stats_t *stats, options_t *opt)
{
	sqfs_inode_generic_t *inode;
	size_t max_blk_count;
	sqfs_u64 filesize;
	sqfs_file_t *file;
	file_info_t *fi;
	int ret;

	if (set_working_dir(opt))
		return -1;

	for (fi = fs->files; fi != NULL; fi = fi->next) {
		if (!opt->cfg.quiet)
			printf("packing %s\n", fi->input_file);

		file = sqfs_open_file(fi->input_file,
				      SQFS_FILE_OPEN_READ_ONLY);
		if (file == NULL) {
			perror(fi->input_file);
			return -1;
		}

		filesize = file->get_size(file);

		max_blk_count = filesize / opt->cfg.block_size;
		if (filesize % opt->cfg.block_size)
			++max_blk_count;

		inode = alloc_flex(sizeof(*inode), sizeof(sqfs_u32),
				   max_blk_count);
		if (inode == NULL) {
			perror("creating file inode");
			file->destroy(file);
			return -1;
		}

		inode->block_sizes = (sqfs_u32 *)inode->extra;
		inode->base.type = SQFS_INODE_FILE;
		sqfs_inode_set_file_size(inode, filesize);
		sqfs_inode_set_frag_location(inode, 0xFFFFFFFF, 0xFFFFFFFF);

		fi->user_ptr = inode;

		ret = write_data_from_file(fi->input_file, data,
					   inode, file, 0);
		file->destroy(file);

		if (ret)
			return -1;

		stats->file_count += 1;
		stats->bytes_read += filesize;
	}

	return restore_working_dir(opt);
}

static int relabel_tree_dfs(const char *filename, sqfs_xattr_writer_t *xwr,
			    tree_node_t *n, void *selinux_handle)
{
	char *path = fstree_get_path(n);
	int ret;

	if (path == NULL) {
		perror("getting absolute node path for SELinux relabeling");
		return -1;
	}

	ret = sqfs_xattr_writer_begin(xwr);
	if (ret) {
		sqfs_perror(filename, "recording xattr key-value pairs", ret);
		return -1;
	}

	if (selinux_relable_node(selinux_handle, xwr, n, path)) {
		free(path);
		return -1;
	}

	ret = sqfs_xattr_writer_end(xwr, &n->xattr_idx);
	if (ret) {
		sqfs_perror(filename, "flushing completed key-value pairs",
			    ret);
		return -1;
	}

	free(path);

	if (S_ISDIR(n->mode)) {
		for (n = n->data.dir.children; n != NULL; n = n->next) {
			if (relabel_tree_dfs(filename, xwr, n, selinux_handle))
				return -1;
		}
	}

	return 0;
}

static int read_fstree(fstree_t *fs, options_t *opt, sqfs_xattr_writer_t *xwr,
		       void *selinux_handle)
{
	FILE *fp;
	int ret;

	if (opt->infile == NULL) {
		return fstree_from_dir(fs, opt->packdir, selinux_handle,
				       xwr, opt->dirscan_flags);
	}

	fp = fopen(opt->infile, "rb");
	if (fp == NULL) {
		perror(opt->infile);
		return -1;
	}

	ret = fstree_from_file(fs, opt->infile, fp);
	fclose(fp);

	if (ret == 0 && selinux_handle != NULL)
		ret = relabel_tree_dfs(opt->cfg.filename, xwr,
				       fs->root, selinux_handle);

	return ret;
}

int main(int argc, char **argv)
{
	int status = EXIT_FAILURE;
	void *sehnd = NULL;
	sqfs_writer_t sqfs;
	options_t opt;

	process_command_line(&opt, argc, argv);

	if (sqfs_writer_init(&sqfs, &opt.cfg))
		return EXIT_FAILURE;

	if (opt.selinux != NULL) {
		sehnd = selinux_open_context_file(opt.selinux);
		if (sehnd == NULL)
			goto out;
	}

	if (read_fstree(&sqfs.fs, &opt, sqfs.xwr, sehnd)) {
		if (sehnd != NULL)
			selinux_close_context_file(sehnd);
		goto out;
	}

	if (sehnd != NULL) {
		selinux_close_context_file(sehnd);
		sehnd = NULL;
	}

	tree_node_sort_recursive(sqfs.fs.root);
	fstree_gen_file_list(&sqfs.fs);

	if (pack_files(sqfs.data, &sqfs.fs, &sqfs.stats, &opt))
		goto out;

	if (sqfs_writer_finish(&sqfs, &opt.cfg))
		goto out;

	status = EXIT_SUCCESS;
out:
	sqfs_writer_cleanup(&sqfs);
	return status;
}
