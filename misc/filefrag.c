/*
 * filefrag.c -- report if a particular file is fragmented
 * 
 * Copyright 2003 by Theodore Ts'o.
 *
 * %Begin-Header%
 * This file may be redistributed under the terms of the GNU Public
 * License.
 * %End-Header%
 */

#ifndef __linux__
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    fputs("This program is only supported on Linux!\n", stderr);
    exit(EXIT_FAILURE);
}
#else
#define _LARGEFILE64_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#ifdef HAVE_GETOPT_H
#include <getopt.h>
#else
extern char *optarg;
extern int optind;
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <sys/ioctl.h>
#include <linux/fd.h>
#include <ext2fs/ext2_types.h>

int verbose = 0;

struct fiemap_extent {
	__u64 fe_offset; /* offset in bytes for the start of the extent */
	__u64 fe_length; /* length in bytes for the extent */
	__u32 fe_flags;  /* returned FIEMAP_EXTENT_* flags for the extent */
	__u32 fe_lun;    /* logical device number for extent (starting at 0) */
};

struct fiemap {
	__u64 fm_start;	 	/* logical starting byte offset (in/out) */
	__u64 fm_length;	/* logical length of map (in/out) */
	__u32 fm_flags;	 	/* FIEMAP_FLAG_* flags for request (in/out) */
	__u32 fm_extent_count;  /* number of extents in fm_extents (in/out) */
	__u64 fm_unused;
	struct fiemap_extent fm_extents[0];
};

#define FIEMAP_FLAG_SYNC	0x00000001 /* sync file data before map */
#define FIEMAP_FLAG_HSM_READ    0x00000002 /* get data from HSM before map */
#define FIEMAP_FLAG_NUM_EXTENTS 0x00000004 /* return only number of extents */
#define FIEMAP_FLAG_INCOMPAT    0xff000000 /* error for unknown flags in here */

#define FIEMAP_EXTENT_HOLE      0x00000001 /* has no data or space allocation */
#define FIEMAP_EXTENT_UNWRITTEN 0x00000002 /* space allocated, but no data */
#define FIEMAP_EXTENT_UNMAPPED  0x00000004 /* has data but no space allocation*/
#define FIEMAP_EXTENT_ERROR     0x00000008 /* mapping error, errno in fe_start*/
#define FIEMAP_EXTENT_NO_DIRECT 0x00000010 /* cannot access data directly */
#define FIEMAP_EXTENT_LAST      0x00000020 /* last extent in the file */
#define FIEMAP_EXTENT_DELALLOC  0x00000040 /* has data but not yet written,
					      must have EXTENT_UNKNOWN set */
#define FIEMAP_EXTENT_SECONDARY 0x00000080 /* data (also) in secondary storage,
					      not in primary if EXTENT_UNKNOWN*/
#define FIEMAP_EXTENT_EOF       0x00000100 /* if fm_start+fm_len is beyond EOF*/


#define FIBMAP		_IO(0x00, 1)	/* bmap access */
#define FIGETBSZ	_IO(0x00, 2)	/* get the block size used for bmap */
#define EXT4_IOC_FIEMAP	_IOWR('f', 10, struct fiemap) /* get file  extent info*/

#define EXT4_EXTENTS_FL			0x00080000 /* Inode uses extents */
#define	EXT3_IOC_GETFLAGS		_IOR('f', 1, long)

static unsigned int div_ceil(unsigned int a, unsigned int b)
{
	if (!a)
		return 0;
	return ((a - 1) / b) + 1;
}

static unsigned long get_bmap(int fd, unsigned long block)
{
	int	ret;
	unsigned int b;

	b = block;
	ret = ioctl(fd, FIBMAP, &b); /* FIBMAP takes a pointer to an integer */
	if (ret < 0) {
		if (errno == EPERM) {
			fprintf(stderr, "No permission to use FIBMAP ioctl; must have root privileges\n");
			exit(1);
		}
		perror("FIBMAP");
	}
	return b;
}

int filefrag_fiemap(int fd, int bs, int *num_extents)
{
	char buf[4096] = "";
	struct fiemap *fiemap = (struct fiemap *)buf;
	int count = (sizeof(buf) - sizeof(*fiemap)) /
			sizeof(struct fiemap_extent);
	__u64 logical_blk = 0, last_blk = 0;
	unsigned long flags;
	int tot_extents = 0;
	int eof = 0;
	int i;
	int rc;

	memset(fiemap, 0, sizeof(struct fiemap));
	fiemap->fm_extent_count = count;
	fiemap->fm_length = ~0ULL;
	if (!verbose)
		flags |= FIEMAP_FLAG_NUM_EXTENTS;

	do {
		fiemap->fm_length = ~0ULL;
		fiemap->fm_flags = flags;
		fiemap->fm_extent_count = count;
		rc = ioctl (fd, EXT4_IOC_FIEMAP, (unsigned long) fiemap);
		if (rc)
			return rc;

		if (!verbose) {
			*num_extents = fiemap->fm_extent_count;
			goto out;
		}

		for (i = 0; i < fiemap->fm_extent_count; i++) {
			__u64 phy_blk;
			unsigned long ext_len;

			phy_blk = fiemap->fm_extents[i].fe_offset / bs;
			ext_len = fiemap->fm_extents[i].fe_length / bs;
			if (logical_blk && (phy_blk != last_blk+1))
				printf("Discontinuity: Block %llu is at %llu "
				       "(was %llu)\n", logical_blk, phy_blk,
				       last_blk);
			logical_blk += ext_len;
			last_blk = phy_blk + ext_len - 1;
			if (fiemap->fm_extents[i].fe_flags & FIEMAP_EXTENT_EOF)
				eof = 1;
		}
		fiemap->fm_start += fiemap->fm_length;
		tot_extents += fiemap->fm_extent_count;
	} while (0);

	*num_extents = tot_extents;
out:
	return 0;
}

#define EXT2_DIRECT	12

static void frag_report(const char *filename)
{
	struct statfs	fsinfo;
#ifdef HAVE_FSTAT64
	struct stat64	fileinfo;
#else
	struct stat	fileinfo;
#endif
	int		bs;
	long		fd;
	unsigned long	block, last_block = 0, numblocks, i;
	long		bpib;	/* Blocks per indirect block */
	long		cylgroups;
	int		num_extents = 0, expected;
	int		is_ext2 = 0;
	unsigned int	flags;

	if (statfs(filename, &fsinfo) < 0) {
		perror("statfs");
		return;
	}
#ifdef HAVE_FSTAT64
	if (stat64(filename, &fileinfo) < 0) {
#else
	if (stat(filename, &fileinfo) < 0) {
#endif
		perror("stat");
		return;
	}
	if (!S_ISREG(fileinfo.st_mode)) {
		printf("%s: Not a regular file\n", filename);
		return;
	}
	if ((fsinfo.f_type == 0xef51) || (fsinfo.f_type == 0xef52) || 
	    (fsinfo.f_type == 0xef53))
		is_ext2++;
	if (verbose) {
		printf("Filesystem type is: %lx\n", 
		       (unsigned long) fsinfo.f_type);
	}
	cylgroups = div_ceil(fsinfo.f_blocks, fsinfo.f_bsize*8);
	if (verbose) {
		printf("Filesystem cylinder groups is approximately %ld\n", 
		       cylgroups);
	}
#ifdef HAVE_OPEN64
	fd = open64(filename, O_RDONLY);
#else
	fd = open(filename, O_RDONLY);
#endif
	if (fd < 0) {
		perror("open");
		return;
	}
	if (ioctl(fd, FIGETBSZ, &bs) < 0) { /* FIGETBSZ takes an int */
		perror("FIGETBSZ");
		close(fd);
		return;
	}
	if (ioctl(fd, EXT3_IOC_GETFLAGS, &flags) < 0)
		flags = 0;
	if (flags & EXT4_EXTENTS_FL) {
		if (verbose)
			printf("File is stored in extents format\n");
		is_ext2 = 0;
	}
	if (verbose)
		printf("Blocksize of file %s is %d\n", filename, bs);
	bpib = bs / 4;
	numblocks = (fileinfo.st_size + (bs-1)) / bs;
	if (verbose) {
		printf("File size of %s is %lld (%ld blocks)\n", filename, 
		       (long long) fileinfo.st_size, numblocks);
		printf("First block: %lu\nLast block: %lu\n",
		       get_bmap(fd, 0), get_bmap(fd, numblocks - 1));
	}
	if (is_ext2 || (filefrag_fiemap(fd, bs, &num_extents) != 0)) {
		for (i = 0; i < numblocks; i++) {
			if (is_ext2 && last_block) {
				if (((i-EXT2_DIRECT) % bpib) == 0)
					last_block++;
				if (((i-EXT2_DIRECT-bpib) % (bpib*bpib)) == 0)
					last_block++;
				if (((i-EXT2_DIRECT-bpib-bpib*bpib) %
							(bpib*bpib*bpib)) == 0)
					last_block++;
			}
			block = get_bmap(fd, i);
			if (block == 0)
				continue;
			if (last_block && (block != last_block+1) ) {
				if (verbose)
					printf("Discontinuity: Block %ld is at "
					       "%lu (was %lu)\n",
					       i, block, last_block+1);
				num_extents++;
			}
			last_block = block;
		}
	}
	if (num_extents == 1)
		printf("%s: 1 extent found", filename);
	else
		printf("%s: %d extents found", filename, num_extents);
	expected = (numblocks/((bs*8)-(fsinfo.f_files/8/cylgroups)-3))+1;
	if (is_ext2 && expected != num_extents)
		printf(", perfection would be %d extent%s\n", expected,
			(expected>1) ? "s" : "");
	else
		fputc('\n', stdout);
	close(fd);
}

static void usage(const char *progname)
{
	fprintf(stderr, "Usage: %s [-v] file ...\n", progname);
	exit(1);
}

int main(int argc, char**argv)
{
	char **cpp;
	int c;

	while ((c = getopt(argc, argv, "v")) != EOF)
		switch (c) {
		case 'v':
			verbose++;
			break;
		default:
			usage(argv[0]);
			break;
		}
	if (optind == argc)
		usage(argv[0]);
	for (cpp=argv+optind; *cpp; cpp++) {
		if (verbose)
			printf("Checking %s\n", *cpp);
		frag_report(*cpp);
	}
	return 0;
}
#endif
