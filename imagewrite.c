/*
 *  imagewrite.c
 *
 *  Copyright (C) 2015 Inteno Broadband Technology AB
 *
 *  Author: Mats Karrman (mats@southpole.se)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Overview:
 *   This utility writes images to nand flash using a procedure similar to
 *   the one implemented by the Broadcom kernel driver.
 *
 * Bug/ToDo:
 */

#define PROGRAM_NAME "imagewrite"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <getopt.h>

#include <asm/types.h>
#include <mtd/mtd-user.h>
#include <mtd/ubi-media.h>
#include <common.h>
#include <crc32.h>
#include <libmtd.h>
#include <mtd_swab.h>


static struct args {
	unsigned long length;
	unsigned long skip;
	unsigned long blocks;
	unsigned long start;
	unsigned long vol_id;
	unsigned long vol_lebs;
	int std_in;
	int clm;
	int ubi;
	int verbose;
	const char *vol_name;
	const char *mtd_device;
	const char *img_file;
} args;

static libmtd_t mtd_desc;

#define HELPTEXT \
"Usage: " PROGRAM_NAME " [OPTION] MTD_DEVICE INPUTFILE\n" \
"\n" \
"Writes data from the specified input file to the specified MTD device.\n" \
"\n" \
"  -b, --blocks=N    Number of eraseblocks to erase/write (default: to end)\n" \
"  -c, --clm         Write JFFS2 clean markers\n" \
"  -i. --stdin       Read input data from STDIN\n" \
"  -k, --skip=N      Offset into input file\n" \
"  -l, --length=N    Length of data to write (default: to end of input file)\n" \
"  -n, --vol-id=N    ID of UBI volume (default: 0)\n" \
"  -N, --vol-name=st Name of UBI volume (mandatory if -u and INPUTFILE used)\n" \
"  -s, --start=N     First eraseblock to erase/write\n" \
"  -S, --vol-lebs=N  Number of LEB's for UBI volume, if N is negative, then\n" \
"                    (all+N-2) blocks are used (default: all-22)\n" \
"  -u, --ubi         Format as UBI device\n" \
"  -q, --quiet       Don't display progress messages\n" \
"  -v, --verbose     Display more progress messages\n" \
"  -h, --help        Display this help and exit\n" \
"  -V, --version     Output version information and exit\n" \
"\n" \
"This program is designed specifically to emulate the functionality of the\n" \
"Broadcom in-kernel flashing routines but without the hazzle of being forced\n" \
"to shut down one kernel. In addition, the program is also able to create\n" \
"UBI devices with contents.\n" \
"\n" \
"Usage examples:\n" \
" # " PROGRAM_NAME " /dev/mtd1 flash.img\n" \
"    Erase the complete mtd1 flash partition and write all of flash.img to it.\n" \
" # " PROGRAM_NAME " -s 3 -b 40 /dev/mtd1\n" \
"    Just erase blocks 3 to 42 of /dev/mtd1.\n" \
" # " PROGRAM_NAME " -c -s 83 -b 408 -k 131072 -l 30408704 /dev/mtd1 flash.img\n" \
"    Erase blocks 83 to 490 of /dev/mtd1 and write bytes 131072 to 30539775 of\n" \
"    flash.img to it, with a JFFS2 clean marker on every written block.\n" \
" # " PROGRAM_NAME " -u -N rootfs -S 100 -s 14 /dev/mtd1 root.ubifs\n" \
"    Erase blocks 14 to end of /dev/mtd1, format as UBI, create a volume named\n" \
"    'rootfs' and write data from root.ubifs to it.\n"

static void display_help(int status)
{
	fprintf(status == EXIT_SUCCESS ? stdout : stderr, HELPTEXT );
	exit(status);
}

static void display_version(void)
{
	printf("%s %s\n", PROGRAM_NAME, VERSION);
	exit(EXIT_SUCCESS);
}

static void process_options(int argc, char * const argv[])
{
	static const char short_options[] = "b:chik:l:n:N:qs:S:uvV";
	static const struct option long_options[] = {
		{"blocks", required_argument, 0, 'b'},
		{"clm", no_argument, 0, 'c'},
		{"help", no_argument, 0, 'h'},
		{"stdin", no_argument, 0, 'i'},
		{"skip", required_argument, 0, 'k'},
		{"length", required_argument, 0, 'l'},
		{"vol-id", required_argument, 0, 'n'},
		{"vol-name", required_argument, 0, 'N'},
		{"quiet", no_argument, 0, 'q'},
		{"start", required_argument, 0, 's'},
		{"vol-lebs", required_argument, 0, 'S'},
		{"ubi", no_argument, 0, 'u'},
		{"verbose", no_argument, 0, 'v'},
		{"version", no_argument, 0, 'V'},
		{0, 0, 0, 0},
	};

	int option_index = 0;
	int ch;
	int err = 0;

	args.verbose = 1;

	for (;;) {
		ch = getopt_long(argc, argv, short_options,
				long_options, &option_index);
		if (ch == EOF)
			break;

		switch (ch) {
			case 'b':
				args.blocks = simple_strtoul(optarg, &err);
				break;
			case 'c':
				args.clm = 1;
				break;
			case 'h':
				display_help(EXIT_SUCCESS);
				break;
			case 'i':
				args.std_in = 1;
				break;
			case 'k':
				args.skip = simple_strtoul(optarg, &err);
				break;
			case 'l':
				args.length = simple_strtoul(optarg, &err);
				break;
			case 'n':
				args.vol_id = simple_strtoul(optarg, &err);
				break;
			case 'N':
				args.vol_name = optarg;
				break;
			case 'q':
				args.verbose = 0;
				break;
			case 's':
				args.start = simple_strtoul(optarg, &err);
				break;
			case 'S':
				args.vol_lebs = simple_strtoul(optarg, &err);
				break;
			case 'u':
				args.ubi = 1;
				break;
			case 'v':
				args.verbose = 2;
				break;
			case 'V':
				display_version();
				break;
			default:
				display_help(EXIT_FAILURE);
				break;
		}

		if (err)
			exit(EXIT_FAILURE);
	}

	argc -= optind;
	argv += optind;

	if (argc < 1 || argc > 2)
		display_help(EXIT_FAILURE);

	args.mtd_device = argv[0];

	if (argc == 2)
		args.img_file = argv[1];
}

static uint32_t gen_image_seq(void)
{
	struct timeval tv;
	struct timezone tz;

	gettimeofday(&tv, &tz);
	srand((tv.tv_sec ^ tv.tv_usec) * getpid() % RAND_MAX);

	return (uint32_t)rand();
}

static int eb_erase(struct mtd_dev_info *mtd, int fd, unsigned long eb_addr)
{
	unsigned long eb;
	int ret;

	eb = eb_addr / mtd->eb_size;
	ret = mtd_is_bad(mtd, fd, eb);
	if (ret > 0) {
		if (args.verbose > 0)
			printf("Skipping erase of bad block at 0x%08lx\n",
				eb_addr);
	} else if (ret < 0) {
		sys_errmsg("Get bad block failed at 0x%08lx", eb_addr);
	} else {
		ret = mtd_erase(mtd_desc, mtd, fd, eb);
		if (ret)
			sys_errmsg("Erase block failed at 0x%08lx", eb_addr);
	}

	return ret;
}

static long data_read(int ifd, unsigned char *dest, unsigned long len)
{
	unsigned long left_to_read = len;
	long data_read;

	while (left_to_read) {
		data_read = read(ifd, dest, left_to_read);
		if (data_read < 0) {
			sys_errmsg("failed to read input data");
			return -1;
		}
		if (!data_read && left_to_read) {
			/* EOF is only OK if reading STDIN without setting
			 * a fixed value for length of input data
			 */
			if (args.std_in && !args.length)
				return (len - left_to_read);
			sys_errmsg("failed to read input data");
			return -1;
		}
		left_to_read -= data_read;
		dest += data_read;
	}

	return len;
}

static long eb_gen_data(struct mtd_dev_info *mtd, int ifd,
			unsigned char *block_buf, unsigned long *data_left)
{
	static unsigned long blk_no = 0;
	static uint32_t image_seq = 0;
	uint32_t crc;
	long data_len;

	memset(block_buf, 0xff, mtd->eb_size);

	if (args.ubi) {
		struct ubi_ec_hdr *ec_hdr;
		struct ubi_vid_hdr *vid_hdr;
		struct ubi_vtbl_record *vtbl_rec;
		unsigned long i;
		unsigned long data_ofs = mtd->min_io_size * 2;

		while (!image_seq)
			image_seq = gen_image_seq();

		/* UBI eraseblock header */
		ec_hdr = (struct ubi_ec_hdr *)&block_buf[0];
		memset(ec_hdr, 0, UBI_EC_HDR_SIZE);
		ec_hdr->magic = cpu_to_be32(UBI_EC_HDR_MAGIC);
		ec_hdr->version = UBI_VERSION;
		ec_hdr->vid_hdr_offset = cpu_to_be32(mtd->min_io_size);
		ec_hdr->data_offset = cpu_to_be32(data_ofs);
		ec_hdr->image_seq = cpu_to_be32(image_seq);
		crc = mtd_crc32(UBI_CRC32_INIT, ec_hdr, UBI_EC_HDR_SIZE_CRC);
		ec_hdr->hdr_crc = cpu_to_be32(crc);

		/* UBI VID header */
		vid_hdr = (struct ubi_vid_hdr *)&block_buf[mtd->min_io_size];
		if (blk_no < UBI_LAYOUT_VOLUME_EBS) {

			/* Volume table LEB */
			memset(vid_hdr, 0, UBI_VID_HDR_SIZE);
			vid_hdr->magic = cpu_to_be32(UBI_VID_HDR_MAGIC);
			vid_hdr->version = UBI_VERSION;
			vid_hdr->vol_type = UBI_LAYOUT_VOLUME_TYPE;
			vid_hdr->compat = UBI_LAYOUT_VOLUME_COMPAT;
			vid_hdr->vol_id = cpu_to_be32(UBI_LAYOUT_VOLUME_ID);
			vid_hdr->lnum = cpu_to_be32(blk_no);
			crc = mtd_crc32(UBI_CRC32_INIT, vid_hdr, UBI_VID_HDR_SIZE_CRC);
			vid_hdr->hdr_crc = cpu_to_be32(crc);

		} else if (blk_no < (args.vol_lebs + UBI_LAYOUT_VOLUME_EBS)) {

			/* Volume data LEB */
			memset(vid_hdr, 0, UBI_VID_HDR_SIZE);
			vid_hdr->magic = cpu_to_be32(UBI_VID_HDR_MAGIC);
			vid_hdr->version = UBI_VERSION;
			vid_hdr->vol_type = UBI_VID_DYNAMIC;
			vid_hdr->vol_id = cpu_to_be32(args.vol_id);
			vid_hdr->lnum = cpu_to_be32(blk_no - UBI_LAYOUT_VOLUME_EBS);
			crc = mtd_crc32(UBI_CRC32_INIT, vid_hdr, UBI_VID_HDR_SIZE_CRC);
			vid_hdr->hdr_crc = cpu_to_be32(crc);
		}

		/* LEB data */
		if (blk_no < UBI_LAYOUT_VOLUME_EBS) {

			/* Volume table data */
			vtbl_rec = (struct ubi_vtbl_record *)&block_buf[data_ofs];
			for (i=0; i<128; ++i) {
				memset(vtbl_rec, 0, UBI_VTBL_RECORD_SIZE);
				if (i == args.vol_id) {
					vtbl_rec->reserved_pebs =
						cpu_to_be32(args.vol_lebs);
					vtbl_rec->alignment = cpu_to_be32(1);
					vtbl_rec->vol_type = UBI_VID_DYNAMIC;
					vtbl_rec->name_len =
						cpu_to_be16(strlen(args.vol_name));
					strcpy((char *)vtbl_rec->name,
						args.vol_name);
				}
				crc = mtd_crc32(UBI_CRC32_INIT, vtbl_rec,
						UBI_VTBL_RECORD_SIZE_CRC);
				vtbl_rec->crc = cpu_to_be32(crc);
				++vtbl_rec;
			}
			data_len = (void *)vtbl_rec - (void *)block_buf;

		} else if (blk_no < (args.vol_lebs + UBI_LAYOUT_VOLUME_EBS)) {

			/* Volume data */
			i = mtd->eb_size - data_ofs;
			data_len = (*data_left > i) ? i : *data_left;
			data_len = data_read(ifd, &block_buf[data_ofs], data_len);
			if (data_len < 0)
				return -1;
			*data_left -= data_len;
			data_len += data_ofs;

		} else {
			/* Erase header only */
			data_len = UBI_EC_HDR_SIZE;
		}

	} else {

		/* Raw data write, no UBI headers */
		data_len = (*data_left > mtd->eb_size) ?
			   mtd->eb_size : *data_left;
		data_len = data_read(ifd, block_buf, data_len);
		if (data_len < 0)
			return -1;
		*data_left -= data_len;
	}

	++blk_no;
	return data_len;
}

static int eb_write(struct mtd_dev_info *mtd, int fd,
		    unsigned long eb_addr, unsigned long data_len,
		    unsigned char *data)
{
	static unsigned char clean_marker[] = {
		0x19, 0x85, 0x20, 0x03, 0x00, 0x00, 0x00, 0x08
	};
	unsigned long write_len;
	unsigned long page_addr, byte;
	unsigned long eb = eb_addr / mtd->eb_size;
	int write_clm = args.clm;
	int ret = 0;

	if (data_len == 0 && !write_clm)
		return 0;

	page_addr = 0;
	while ((page_addr < data_len) || write_clm)
	{
		write_len = 0;
		for (byte = 0; byte < mtd->min_io_size; ++byte) {
			if (data[page_addr + byte] != 0xFF) {
				write_len = mtd->min_io_size;
				break;
			}
		}

		ret = mtd_write(
			mtd_desc, mtd, fd,
			eb,
			page_addr,
			write_len ? &data[page_addr] : NULL,
			write_len,
			write_clm ? clean_marker : NULL,
			write_clm ? sizeof(clean_marker) : 0,
			MTD_OPS_AUTO_OOB
		);
		if (ret) {
			sys_errmsg("Write page failed at 0x%08lx",
				eb_addr + page_addr);
			eb_erase(mtd, fd, eb_addr);
			if (data_len % mtd->eb_size == 0)
				mtd_mark_bad(mtd, fd, eb);
			break;
		}

		write_clm = 0;  /* Clean marker on first page only */
		page_addr += mtd->min_io_size;
	}

	return ret;
}

int main(int argc, char *argv[])
{
	struct stat st;
	struct mtd_dev_info mtd;
	int fd = -1;
	int ifd = -1;
	unsigned long image_size;
	unsigned char *block_buf = NULL;
	unsigned long start;
	unsigned long end;
	unsigned long eb_addr;
	unsigned long data_left;
	long data_len;
	int failed = 1;

	process_options(argc, argv);

	if (args.img_file && args.std_in)
		errmsg_die("can't have both --stdin and an input file");

	if (args.ubi && (args.img_file || args.std_in) && !args.vol_name)
		errmsg_die("--ubi and input data require --vol-name");

	if ((fd = open(args.mtd_device, O_RDWR)) == -1)
		sys_errmsg_die("%s", args.mtd_device);

	mtd_desc = libmtd_open();
	if (!mtd_desc)
		errmsg_die("failed to initialize libmtd");

	if (mtd_get_dev_info(mtd_desc, args.mtd_device, &mtd) < 0)
		errmsg_die("failed to get mtd device info");

	start = args.start * mtd.eb_size;
	end = args.blocks ? start + args.blocks * mtd.eb_size : mtd.size;

	if (start > (mtd.size - mtd.eb_size))
		errmsg_die("start block out of range");

	if (end > mtd.size)
		errmsg_die("block count out of range");

	if (args.img_file || args.std_in || args.ubi) {
		block_buf = xmalloc(mtd.eb_size);
		if (!block_buf) {
			errmsg("failed to allocate memory");
			goto closeall;
		}
	}

	if (args.std_in) {

		if (args.skip) {
			errmsg("--skip not supported with --stdin");
			goto closeall;
		}

		ifd = STDIN_FILENO;
		image_size = args.length;

	} else if (args.img_file) {

		ifd = open(args.img_file, O_RDONLY);
		if (ifd == -1) {
			sys_errmsg("failed to open image file");
			goto closeall;
		}

		if (fstat(ifd, &st)) {
			sys_errmsg("failed to stat image file");
			goto closeall;
		}

		if (args.skip + args.length > st.st_size) {
			errmsg("image file is too small");
			goto closeall;
		}

		image_size = args.length ? args.length : st.st_size - args.skip;

		if (args.skip) {
			if (lseek(ifd, args.skip, SEEK_SET) != args.skip) {
				sys_errmsg("failed to seek input file");
				goto closeall;
			}
		}

	} else {

		if (args.length || args.skip)
			errmsg_die("can't have --skip or --length without"
				   " input file");
		image_size = 0;
	}

	if (args.ubi) {
		unsigned long leb_size;
		long tot_lebs;
		long vol_lebs;

		leb_size = mtd.eb_size - (mtd.min_io_size * 2);
		tot_lebs = (end - start) / mtd.eb_size - UBI_LAYOUT_VOLUME_EBS;

		vol_lebs = (long)args.vol_lebs;
		if (vol_lebs == 0)
			vol_lebs = tot_lebs - 20;  /* For bad block handling */
		else if (vol_lebs < 0)
			vol_lebs = tot_lebs + args.vol_lebs;
		else
			vol_lebs = args.vol_lebs;

		if (vol_lebs < 0 || vol_lebs > tot_lebs) {
			errmsg("volume LEBs doesn't fit into allocated blocks");
			goto closeall;
		}


		if (image_size > (vol_lebs * leb_size)) {
			errmsg("image file does not fit into allocated LEBs");
			goto closeall;
		}

		args.vol_lebs = (unsigned long)vol_lebs;

		if (strlen(args.vol_name) > UBI_VOL_NAME_MAX) {
			errmsg("volume name too long");
			goto closeall;
		}
	} else {
		if (image_size > (end - start)) {
			errmsg("image file does not fit into allocated blocks");
			goto closeall;
		}
	}

	if (args.verbose > 0)
		printf("Erasing all blocks from 0x%08lx to 0x%08lx\n",
			start, end);
	for (eb_addr = start; eb_addr < end; eb_addr += mtd.eb_size)
		eb_erase(&mtd, fd, eb_addr);

	if (!image_size && !args.std_in && !args.ubi) {
		failed = 0;
		goto closeall;
	}

	if (args.verbose == 1)
		printf("Writing blocks from 0x%08lx to 0x%08lx\n",
			start, end);

	data_left = image_size ? image_size : (unsigned long)-1;
	for (eb_addr = start; eb_addr < end; eb_addr += mtd.eb_size) {

		data_len = eb_gen_data(&mtd, ifd, block_buf, &data_left);
		if (data_len < 0)
			break;

		while (eb_addr < end) {
			if (args.verbose > 1) {
				printf("\rWriting block at 0x%08lx", eb_addr);
				fflush(stdout);
			} else if (args.verbose > 0) {
				printf(".");
				fflush(stdout);
			}

			if (eb_write(&mtd, fd, eb_addr, data_len, block_buf) == 0)
				break;

			eb_addr += mtd.eb_size;
		}
	}

	if (args.verbose > 0)
		printf("\n");

	if (!data_left || !image_size)
		failed = 0;

closeall:
	close(ifd);
	libmtd_close(mtd_desc);
	free(block_buf);
	close(fd);

	if (failed)
		errmsg_die("data only partially written due to error");

	return EXIT_SUCCESS;
}

