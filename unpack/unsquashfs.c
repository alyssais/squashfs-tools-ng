/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "unsquashfs.h"

enum {
	OP_NONE = 0,
	OP_LS,
	OP_CAT,
	OP_UNPACK,
};

static struct option long_opts[] = {
	{ "list", required_argument, NULL, 'l' },
	{ "cat", required_argument, NULL, 'c' },
	{ "unpack-root", required_argument, NULL, 'u' },
	{ "no-dev", no_argument, NULL, 'D' },
	{ "no-sock", no_argument, NULL, 'S' },
	{ "no-fifo", no_argument, NULL, 'F' },
	{ "no-slink", no_argument, NULL, 'L' },
	{ "no-empty-dir", no_argument, NULL, 'E' },
	{ "chmod", no_argument, NULL, 'C' },
	{ "chown", no_argument, NULL, 'O' },
	{ "help", no_argument, NULL, 'h' },
	{ "version", no_argument, NULL, 'V' },
};

static const char *short_opts = "l:c:u:DSFLCOEhV";

static const char *help_string =
"Usage: %s [OPTIONS] <squashfs-file>\n"
"\n"
"View or extract the contents of a squashfs image.\n"
"\n"
"Possible options:\n"
"  --list, -l <path>     Produce a directory listing for a given path in the\n"
"                        squashfs image.\n"
"  --cat, -c <path>      If the specified path is a regular file in the,\n"
"                        image, dump its contents to stdout.\n"
"  --unpack-root <path>  Unpack the contents of the filesystem into the\n"
"                        specified path.\n"
"  --no-dev, -D          Do not unpack device special files.\n"
"  --no-sock, -S         Do not unpack socket files.\n"
"  --no-fifo, -F         Do not unpack named pipes.\n"
"  --no-slink, -L        Do not unpack symbolic links.\n"
"  --no-empty-dir, -E    Do not unpack directories that would end up empty.\n"
"  --chmod, -C           Change permission flags of unpacked files to those\n"
"                        store in the squashfs image.\n"
"  --chown, -O           Change ownership of unpacked files to the UID/GID\n"
"                        set in the squashfs iamge.\n"
"\n"
"  --help, -h            Print help text and exit.\n"
"  --version, -V         Print version information and exit.\n"
"\n";

extern const char *__progname;

static tree_node_t *find_node(tree_node_t *n, const char *path)
{
	const char *end;
	size_t len;

	while (path != NULL && *path != '\0') {
		if (!S_ISDIR(n->mode)) {
			errno = ENOTDIR;
			return NULL;
		}

		end = strchrnul(path, '/');
		len = end - path;

		for (n = n->data.dir->children; n != NULL; n = n->next) {
			if (strncmp(path, n->name, len) != 0)
				continue;
			if (n->name[len] != '\0')
				continue;
			break;
		}

		if (n == NULL) {
			errno = ENOENT;
			return NULL;
		}

		path = *end ? (end + 1) : end;
	}

	return n;
}

static char *get_path(char *old, const char *arg)
{
	char *path;

	free(old);

	path = strdup(arg);
	if (path == NULL) {
		perror("processing arguments");
		exit(EXIT_FAILURE);
	}

	if (canonicalize_name(path)) {
		fprintf(stderr, "Invalid path: %s\n", arg);
		free(path);
		exit(EXIT_FAILURE);
	}

	return path;
}

int main(int argc, char **argv)
{
	int i, fd, status = EXIT_FAILURE, op = OP_NONE, unpack_flags = 0;
	const char *unpack_root = NULL;
	frag_reader_t *frag = NULL;
	char *cmdpath = NULL;
	sqfs_super_t super;
	compressor_t *cmp;
	tree_node_t *n;
	fstree_t fs;

	for (;;) {
		i = getopt_long(argc, argv, short_opts, long_opts, NULL);
		if (i == -1)
			break;

		switch (i) {
		case 'D':
			unpack_flags |= UNPACK_NO_DEVICES;
			break;
		case 'S':
			unpack_flags |= UNPACK_NO_SOCKETS;
			break;
		case 'F':
			unpack_flags |= UNPACK_NO_FIFO;
			break;
		case 'L':
			unpack_flags |= UNPACK_NO_SLINKS;
			break;
		case 'C':
			unpack_flags |= UNPACK_CHMOD;
			break;
		case 'O':
			unpack_flags |= UNPACK_CHOWN;
			break;
		case 'E':
			unpack_flags |= UNPACK_NO_EMPTY;
			break;
		case 'c':
			op = OP_CAT;
			cmdpath = get_path(cmdpath, optarg);
			break;
		case 'l':
			op = OP_LS;
			cmdpath = get_path(cmdpath, optarg);
			break;
		case 'u':
			op = OP_UNPACK;
			unpack_root = optarg;
			break;
		case 'h':
			printf(help_string, __progname);
			status = EXIT_SUCCESS;
			goto out_cmd;
		case 'V':
			print_version();
			status = EXIT_SUCCESS;
			goto out_cmd;
		default:
			goto fail_arg;
		}
	}

	if (op == OP_NONE) {
		fputs("No opteration specified\n", stderr);
		goto fail_arg;
	}

	if (optind >= argc) {
		fputs("Missing image argument\n", stderr);
		goto fail_arg;
	}

	fd = open(argv[optind], O_RDONLY);
	if (fd < 0) {
		perror(argv[optind]);
		goto out_cmd;
	}

	if (sqfs_super_read(&super, fd))
		goto out_fd;

	if ((super.version_major != SQFS_VERSION_MAJOR) ||
	    (super.version_minor != SQFS_VERSION_MINOR)) {
		fprintf(stderr,
			"The image uses squashfs version %d.%d\n"
			"We currently only supports version %d.%d (sorry).\n",
			super.version_major, super.version_minor,
			SQFS_VERSION_MAJOR, SQFS_VERSION_MINOR);
		goto out_fd;
	}

	if (super.flags & SQFS_FLAG_COMPRESSOR_OPTIONS) {
		fputs("Image has been built with compressor options.\n"
		      "This is not yet supported.\n",
		      stderr);
		goto out_fd;
	}

	if (!compressor_exists(super.compression_id)) {
		fputs("Image uses a compressor that has not been built in\n",
		      stderr);
		goto out_fd;
	}

	cmp = compressor_create(super.compression_id, false, super.block_size);
	if (cmp == NULL)
		goto out_fd;

	if (read_fstree(&fs, fd, &super, cmp, unpack_flags))
		goto out_cmp;

	switch (op) {
	case OP_LS:
		n = find_node(fs.root, cmdpath);
		if (n == NULL) {
			perror(cmdpath);
			goto out_fs;
		}

		list_files(n);
		break;
	case OP_CAT:
		n = find_node(fs.root, cmdpath);
		if (n == NULL) {
			perror(cmdpath);
			goto out_fs;
		}

		if (!S_ISREG(n->mode)) {
			fprintf(stderr, "/%s: not a regular file\n", cmdpath);
			goto out_fs;
		}

		if (super.fragment_entry_count > 0 &&
		    super.fragment_table_start < super.bytes_used &&
		    !(super.flags & SQFS_FLAG_NO_FRAGMENTS)) {
			frag = frag_reader_create(&super, fd, cmp);
			if (frag == NULL)
				goto out_fs;
		}

		if (extract_file(n->data.file, cmp, super.block_size,
				 frag, fd, STDOUT_FILENO)) {
			goto out_fs;
		}
		break;
	case OP_UNPACK:
		if (super.fragment_entry_count > 0 &&
		    super.fragment_table_start < super.bytes_used &&
		    !(super.flags & SQFS_FLAG_NO_FRAGMENTS)) {
			frag = frag_reader_create(&super, fd, cmp);
			if (frag == NULL)
				goto out_fs;
		}

		if (restore_fstree(unpack_root, fs.root, cmp, super.block_size,
				   frag, fd, unpack_flags)) {
			goto out_fs;
		}
		break;
	}

	status = EXIT_SUCCESS;
out_fs:
	if (frag != NULL)
		frag_reader_destroy(frag);
	fstree_cleanup(&fs);
out_cmp:
	cmp->destroy(cmp);
out_fd:
	close(fd);
out_cmd:
	free(cmdpath);
	return status;
fail_arg:
	fprintf(stderr, "Try `%s --help' for more information.\n", __progname);
	free(cmdpath);
	return EXIT_FAILURE;
}