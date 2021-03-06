/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * fstree.h
 *
 * Copyright (C) 2019 David Oberhollenzer <goliath@infraroot.at>
 */
#ifndef FSTREE_H
#define FSTREE_H

#include "config.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#include "sqfs/predef.h"
#include "util/compat.h"

typedef struct tree_node_t tree_node_t;
typedef struct file_info_t file_info_t;
typedef struct dir_info_t dir_info_t;
typedef struct fstree_t fstree_t;

/* Additional meta data stored in a tree_node_t for regular files. */
struct file_info_t {
	/* Linked list pointer for files in fstree_t */
	file_info_t *next;

	/* Path to the input file. */
	char *input_file;

	void *user_ptr;
};

/* Additional meta data stored in a tree_node_t for directories */
struct dir_info_t {
	/* Linked list head for children in the directory */
	tree_node_t *children;

	/* Set to true for implicitly generated directories.  */
	bool created_implicitly;
};

/* A node in a file system tree */
struct tree_node_t {
	/* Parent directory children linked list pointer. */
	tree_node_t *next;

	/* Root node has this set to NULL. */
	tree_node_t *parent;

	/* For the root node, this points to an empty string. */
	char *name;

	sqfs_u32 xattr_idx;
	sqfs_u32 uid;
	sqfs_u32 gid;
	sqfs_u32 inode_num;
	sqfs_u32 mod_time;
	sqfs_u16 mode;
	sqfs_u16 pad0;

	/* SquashFS inode refernce number. 32 bit offset of the meta data
	   block start (relative to inode table start), shifted left by 16
	   and ored with a 13 bit offset into the uncompressed meta data block.

	   Generated on the fly when writing inodes. */
	sqfs_u64 inode_ref;

	/* Type specific data. Pointers are into payload area blow. */
	union {
		dir_info_t dir;
		file_info_t file;
		char *slink_target;
		sqfs_u64 devno;
	} data;

	sqfs_u8 payload[];
};

/* Encapsulates a file system tree */
struct fstree_t {
	struct stat defaults;
	size_t block_size;
	size_t inode_tbl_size;

	tree_node_t *root;

	/* linear array of tree nodes. inode number is array index */
	tree_node_t **inode_table;

	/* linear linked list of all regular files */
	file_info_t *files;
};

/*
  Initializing means copying over the default values and creating a root node.
  On error, an error message is written to stderr.

  The string `defaults` can specify default attributes (mode, uid, gid, mtime)
  as a comma seperated list of key value paris (<key>=<value>[,...]). The string
  is passed to getsubopt and will be altered.

  Returns 0 on success.
*/
int fstree_init(fstree_t *fs, char *defaults);

void fstree_cleanup(fstree_t *fs);

/*
  Create a tree node from a struct stat, node name and extra data.

  For symlinks, the extra part is interpreted as target. For regular files, it
  is interpreted as input path (can be NULL). The name doesn't have to be null
  terminated, a length has to be specified.

  This function does not print anything to stderr, instead it sets an
  appropriate errno value.

  The resulting node can be freed with a single free() call.
*/
tree_node_t *fstree_mknode(tree_node_t *parent, const char *name,
			   size_t name_len, const char *extra,
			   const struct stat *sb);

/*
  Add a node to an fstree at a specific path.

  If some components of the path don't exist, they are created as directories
  with default permissions, like mkdir -p would, and marked as implcitily
  created. A subsequent call that tries to create an existing tree node will
  fail, except if the target is an implicitly created directory node and the
  call tries to create it as a directory (this will simply overwrite the
  permissions and ownership). The implicitly created flag is then cleared.
  Subsequent attempts to create an existing directory again will then also
  fail.

  This function does not print anything to stderr, instead it sets an
  appropriate errno value. Internally it uses fstree_mknode to create the node.
*/
tree_node_t *fstree_add_generic(fstree_t *fs, const char *path,
				const struct stat *sb, const char *extra);

/*
  Parses the file format accepted by gensquashfs and produce a file system
  tree from it. File input paths are interpreted as relative to the current
  working directory.

  Data is read from the given file pointer. The filename is only used for
  producing error messages.

  On failure, an error report with filename and line number is written
  to stderr.

  Returns 0 on success.
 */
int fstree_from_file(fstree_t *fs, const char *filename, FILE *fp);

/* Returns 0 on success. Prints to stderr on failure */
int fstree_gen_inode_table(fstree_t *fs);

void fstree_gen_file_list(fstree_t *fs);

/*
  Generate a string holding the full path of a node. Returned
  string must be freed.

  Returns NULL on failure and sets errno.
*/
char *fstree_get_path(tree_node_t *node);

/* ASCIIbetically sort a linked list of tree nodes */
tree_node_t *tree_node_list_sort(tree_node_t *head);

/* ASCIIbetically sort all sub directories recursively */
void tree_node_sort_recursive(tree_node_t *root);

/*
  If the environment variable SOURCE_DATE_EPOCH is set to a parsable number
  that fits into an unsigned 32 bit value, return its value. Otherwise,
  default to 0.
 */
sqfs_u32 get_source_date_epoch(void);

#endif /* FSTREE_H */
