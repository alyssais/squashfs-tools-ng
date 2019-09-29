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
		if (!opt->quiet)
			printf("packing %s\n", fi->input_file);

		file = sqfs_open_file(fi->input_file,
				      SQFS_FILE_OPEN_READ_ONLY);
		if (file == NULL) {
			perror(fi->input_file);
			return -1;
		}

		filesize = file->get_size(file);

		max_blk_count = filesize / opt->blksz;
		if (filesize % opt->blksz)
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

		ret = write_data_from_file(data, inode, file, 0);
		file->destroy(file);

		if (ret)
			return -1;

		stats->file_count += 1;
		stats->bytes_read += filesize;
	}

	if (sqfs_data_writer_finish(data))
		return -1;

	return restore_working_dir(opt);
}

static int relabel_tree_dfs(sqfs_xattr_writer_t *xwr, tree_node_t *n,
			    void *selinux_handle)
{
	char *path = fstree_get_path(n);

	if (path == NULL) {
		perror("getting absolute node path for SELinux relabeling");
		return -1;
	}

	if (sqfs_xattr_writer_begin(xwr)) {
		fputs("error recoding xattr key-value pairs\n", stderr);
		return -1;
	}

	if (selinux_relable_node(selinux_handle, xwr, n, path)) {
		free(path);
		return -1;
	}

	if (sqfs_xattr_writer_end(xwr, &n->xattr_idx)) {
		fputs("error generating xattr index\n", stderr);
		return -1;
	}

	free(path);

	if (S_ISDIR(n->mode)) {
		for (n = n->data.dir.children; n != NULL; n = n->next) {
			if (relabel_tree_dfs(xwr, n, selinux_handle))
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
		ret = relabel_tree_dfs(xwr, fs->root, selinux_handle);

	return ret;
}

int main(int argc, char **argv)
{
	int status = EXIT_FAILURE, ret;
	sqfs_compressor_config_t cfg;
	data_writer_stats_t stats;
	sqfs_data_writer_t *data;
	sqfs_xattr_writer_t *xwr;
	sqfs_compressor_t *cmp;
	sqfs_id_table_t *idtbl;
	sqfs_file_t *outfile;
	void *sehnd = NULL;
	sqfs_super_t super;
	options_t opt;
	fstree_t fs;

	process_command_line(&opt, argc, argv);

	if (compressor_cfg_init_options(&cfg, opt.compressor,
					opt.blksz, opt.comp_extra)) {
		return EXIT_FAILURE;
	}

	if (fstree_init(&fs, opt.fs_defaults))
		return EXIT_FAILURE;

	if (sqfs_super_init(&super, opt.blksz, fs.defaults.st_mtime,
			    opt.compressor)) {
		goto out_fstree;
	}

	idtbl = sqfs_id_table_create();
	if (idtbl == NULL)
		goto out_fstree;

	outfile = sqfs_open_file(opt.outfile, opt.outmode);
	if (outfile == NULL) {
		perror(opt.outfile);
		goto out_idtbl;
	}

	if (sqfs_super_write(&super, outfile))
		goto out_outfile;

	if (opt.selinux != NULL) {
		sehnd = selinux_open_context_file(opt.selinux);
		if (sehnd == NULL)
			goto out_outfile;
	}

	xwr = sqfs_xattr_writer_create();
	if (xwr == NULL) {
		perror("creating Xattr writer");
		goto out_outfile;
	}

	if (read_fstree(&fs, &opt, xwr, sehnd)) {
		if (sehnd != NULL)
			selinux_close_context_file(sehnd);
		goto out_xwr;
	}

	if (sehnd != NULL) {
		selinux_close_context_file(sehnd);
		sehnd = NULL;
	}

	tree_node_sort_recursive(fs.root);

	if (fstree_gen_inode_table(&fs))
		goto out_xwr;

	fstree_gen_file_list(&fs);

	super.inode_count = fs.inode_tbl_size;

	cmp = sqfs_compressor_create(&cfg);
	if (cmp == NULL) {
		fputs("Error creating compressor\n", stderr);
		goto out_xwr;
	}

	ret = cmp->write_options(cmp, outfile);
	if (ret < 0)
		goto out_cmp;

	if (ret > 0)
		super.flags |= SQFS_FLAG_COMPRESSOR_OPTIONS;

	data = sqfs_data_writer_create(super.block_size, cmp, opt.num_jobs,
				       opt.max_backlog, opt.devblksz, outfile);
	if (data == NULL)
		goto out_cmp;

	memset(&stats, 0, sizeof(stats));
	register_stat_hooks(data, &stats);

	if (pack_files(data, &fs, &stats, &opt))
		goto out_data;

	if (sqfs_serialize_fstree(outfile, &super, &fs, cmp, idtbl))
		goto out_data;

	if (sqfs_data_writer_write_fragment_table(data, &super))
		goto out_data;

	if (opt.exportable) {
		if (write_export_table(outfile, &fs, &super, cmp))
			goto out_data;
	}

	if (sqfs_id_table_write(idtbl, outfile, &super, cmp))
		goto out_data;

	if (sqfs_xattr_writer_flush(xwr, outfile, &super, cmp)) {
		fputs("Error writing xattr table\n", stderr);
		goto out_data;
	}

	super.bytes_used = outfile->get_size(outfile);

	if (sqfs_super_write(&super, outfile))
		goto out_data;

	if (padd_sqfs(outfile, super.bytes_used, opt.devblksz))
		goto out_data;

	if (!opt.quiet)
		sqfs_print_statistics(&super, &stats);

	status = EXIT_SUCCESS;
out_data:
	sqfs_data_writer_destroy(data);
out_cmp:
	cmp->destroy(cmp);
out_xwr:
	sqfs_xattr_writer_destroy(xwr);
out_outfile:
	outfile->destroy(outfile);
out_idtbl:
	sqfs_id_table_destroy(idtbl);
out_fstree:
	fstree_cleanup(&fs);
	return status;
}
