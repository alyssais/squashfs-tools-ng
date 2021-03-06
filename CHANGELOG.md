# Changelog
All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.7]
### Added
- LGPLv3 licensed, shared library `libsquashfs.so` containing all the SquashFS
  related logic.
- Sanitized, public headers and pkg-config file for libsquashfs.
- Doxygen reference manual for libsquashfs.
- Legacy LZMA compression support.
- User configurable queue backlog for tar2sqfs and gensquashfs.

### Changed
- Make sqfsdiff continue comparing even if the types are different,
  but compatible (e.g. extended directory vs basic directory).
- Try to determine the number of available CPU cores and use the
  maximum by default.
- Start numbering inodes at 1, instead of 2.
- Only store permission bits in inodes, the reader reconstructs them from the
  inode type.
- Make "--keep-time" the default for tar2sqfs and use flag to disable it.

### Fixed
- An off-by-one error in the directory packing code.
- Typo in configure fallback path searching for LZO library.
- Typo that caused LZMA2 VLI filters to not be used at all.
- Possible out-of-bounds access in LZO compressor constructor.
- Inverted logic in sqfs2tar extended attributes processing.

### Removed
- Comparisong with directory from sqfsdiff.

## [0.6.1] - 2019-08-27
### Added
- Add a change log
- Add test programs for fuzzing tar and gensquashfs file format parsers

### Fixed
- Harden against integer overflows when parsing squashfs images
- Test against format limits when parsing directory entries
- More thorough bounds checking when reading metadata blocks

## [0.6.0] - 2019-08-22
### Added
- New utility `sqfsdiff` that can compare squashfs images
- rdsquashfs can now dump extended attributes for an inode
- rdsquashfs can now optionally set xattrs on unpacked files
- rdsquashfs can now optionally restore timestamps on unpacked files
- sqfs2tar can now optionally copy xattrs over to the resulting tarball
- gensquashfs can now optionally read xattrs from input files
- gensquashfs now has a --one-file-system option
- tar2sqfs and gensquashfs now output some simple statistics
- Full fragment and data block deduplication
- Support for SOURCE_DATE_EPOCH environment variable
- Optimized, faster file unpacking order
- Faster, pthread based, parallel block compressor

### Fixed
- Return the correct value from data_reader_create
- Fix free() of stack pointer in id_table_read error path
- Fix missing initialization of file fragment fields
- Fix xattr OOL position
- Fix super block flags: clear "no xattr" flag when writing xattrs
- Fix xattr writer size accounting
- Fix explicit NULL dereference in deserialize_fstree failure path
- Fix tar header error reporting on 32 bit systems
- Make sure file listing generated by rdsquashfs -d is properly escaped
- Fix functions with side effect being used inside asserts
- Fix zero padding of extracted data blocks
- Fix forward seek when unpacking sparse files
- Fix wrong argument type for gensquashfs --keep-time
- Fix memory leak in dir-scan error code path
- Fix chmod of symlinks in restore_fstree
- Add proper copyright headers to all source files

### Changed
- Various internal data structure and code cleanups
- Overhaul README and convert it to markdown

## [0.5.0] - 2019-07-28
### Added
- Support for NFS export
- Support for xattr value deduplication
- Flag in packers to optionally keep the original time stamps
- Largefile support
- Implement simple, fork() based parallel unpacking in rdsquashfs

### Fixed
- Remove unfriendly words about squashfs-tools from README
- Propper error message for ZSTD compressor
- Correct copy-and-paste mistake in the build system
- Make sure xattr string table is propperly initialized
- More lenient tar checksum verification
- Fix xattr unit test
- Fix possible leak in tar2sqfs if writing xattrs fails
- Fix corner cases in directory list parsing
- Fix processing of tar mtime on 32 bit systems
- libfstree: fix signed/unsigned comparisons
- Fix fragment reader out of bounds read when loading table
- Fix checks of super block block size
- Fix potential resource leak in deserialize_tree
- Enforce reasonable upper and low bounds on the size of tar headers
- Make sure target in fstree_mknode is always set when creating a symlink
- Use safer string copy function to fill tar header

## [0.4.2] - 2019-07-17
### Fixed
- Sanity check id table size read from super block
- Various bug fixes through Coverity scan
- Fix dirindex writing for ext dir inode
- fstree: mknode: initialize fragment data, add extra blocksize slot
- Fix directory index creation
- Support for reading files without fragments
- Support for spaces in filenames
- Eleminate use of temporary file

## [0.4.1] - 2019-07-07
### Fixed
- read_inode: determine mode bits from inode type
- Actually encode/decode directory inode difference as signed
- Fix regression in fstree_from_file device node format
- Always initialize gensquashfs defaults option

## [0.4.0] - 2019-07-04
### Added
- no-skip option in tar2sqfs and sqfs2tar
- Option for sqfs2tar to extract only some subdirectories
- Support for xattr extensions in tar parser
- Support repacking xattrs from tarball into squashfs filesystem

### Fixed
- Null-pointer dereference in restore_unpack
- Memory leak in gzip compressor
- Stack pointer free() in fstree_from_dir
- Use of uninitialized xattr structure
- Initialize return status in fstree_relabel_selinux
- Make pax header parser bail if parsing a number fails
- Double free in GNU tar sparse file parser
- Never used overflow error message in fstree_from_file
- Unused variable assignment in tar header writer
- Make sure fragment and id tables are initialized
- Directory index offset calculation
- Missing htole32 transformations
- Don't blindly strcpy() the tar header name
- Typos in README
- Composition order of prefix + name for ustar
- Actually check return value when writing xattrs
- Possible out of bounds read in libcompress.a
- Check block_log range before deriving block size from it
- tar2sqfs: check for invalid file names first

### Changed
- Tar writer: Use more widely supported GNU tar extensions instead of PAX
- Simplify deduction logic for squashfs inode type

## [0.3.0] - 2019-06-30
### Added
- Add utility to turn a squashfs image into a POSIX tar archvie
- Add utility to turn a POSIX/PAX tar archive into a squashfs image
- Add unit tests
- Add support for packing sparse files
- Add support for unpacking sparse files as sparse files

### Fixed
- Actually update permissions in fstree add by path
- Always set permissions on symlinks to 0777
- gensquashfs: Fix typo in help text
- Fix inode fragment & sparse counter initialization
- Ommit fragment table if there really are no fragments

### Changed
- Lots of internal cleanups and restructuring

## [0.2.0] - 2019-06-13
### Fixed
- Make empty directories with xattrs work
- Flush the last, unfinished fragment block

### Changed
- Add pushd/popd utility functions and replace directory traversal code
- Lots of internal cleanups
- Use abstractions for many operations and move them to support libraries

## [0.1.0] - 2019-06-08
### Added
- Salvage protoype from Pygos project and turn it into generic squashfs packer
- Add unpacker

### Changed
- Insert abstraction layers and split generic code off into support libraries
