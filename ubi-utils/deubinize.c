/*
 * Copyright (C) 2015 Inteno Broadband Technology AB
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See
 * the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/*
 * Extract contents from UBI images.
 *
 * Author: Mats Kärrman <mats@southpole.se>
 */

#define PROGRAM_NAME    "deubinize"
#define VERSION         "1.0"

#include <getopt.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <crc32.h>
#include <mtd/ubi-media.h>
#include <mtd_swab.h>
#include "common.h"
#include "ubiutils-common.h"


#ifdef DEBUG
# undef DEBUG
# define DEBUG(args)	do { printf args; } while (0)
#else
# define DEBUG(args)
#endif


static const char doc[] = PROGRAM_NAME " version " VERSION
" - a tool to extract the contents,\n"
"i.e. the raw binary image data, from one UBI volume of an UBI image.\n";

static const char optionsstr[] =
"-o, --output=<file name>  output file name\n"
"-p, --peb-size=<bytes>    size of the physical eraseblock of the flash\n"
"                          this UBI image was created for in bytes,\n"
"                          kilobytes (KiB), or megabytes (MiB)\n"
"                          (mandatory parameter)\n"
"-i, --vol-index=<index>   volume table index of volume to extract\n"
"-n, --vol-name=<name>     name of volume to extract\n"
"-s, --skip-bad-blocks     skip eraseblocks with broken headers when\n"
"                          reading data\n"
"-v, --verbose             be verbose\n"
"-h, --help                print help message\n"
"-V, --version             print program version";

static const char usage[] =
"Usage: " PROGRAM_NAME " [-o <file name>] [-p <bytes>] [-i <index>|-n <name>] [-s] ubi-file\n"
"Example: " PROGRAM_NAME " -o ubifs.img -p 128KiB -n root_fs ubi.img\n"
"- extract contents of volume named 'root_fs' from 'ubi.img' to file 'ubifs.img'";

static const struct option long_options[] = {
	{ .name = "output",          .has_arg = 1, .flag = NULL, .val = 'o' },
	{ .name = "peb-size",        .has_arg = 1, .flag = NULL, .val = 'p' },
	{ .name = "vol-index",       .has_arg = 1, .flag = NULL, .val = 'i' },
	{ .name = "vol-name",        .has_arg = 1, .flag = NULL, .val = 'n' },
	{ .name = "skip-bad-blocks", .has_arg = 0, .flag = NULL, .val = 's' },
	{ .name = "verbose",         .has_arg = 0, .flag = NULL, .val = 'v' },
	{ .name = "help",            .has_arg = 0, .flag = NULL, .val = 'h' },
	{ .name = "version",         .has_arg = 0, .flag = NULL, .val = 'V' },
	{ NULL, 0, NULL, 0 }
};


struct img_info {
	off_t size;
	__be32 vid_hdr_offset;
	__be32 data_offset;
	__be32 vol_id;
	__be32 lnum;		/* logical eraseblock number */
};

struct args {
	const char *f_in;
	const char *f_out;
	int peb_size;
	int vol_index;
	const char *vol_name;
	int skip_bad;
	int verbose;
};


static struct args args = {
	.peb_size     = -1
};


static int parse_opt(int argc, char * const argv[])
{
	int index_set = 0;
	int key;

	for (;;) {
		key = getopt_long(argc, argv, "o:p:i:n:svhV", long_options, NULL);
		if (key == -1)
			break;

		switch (key) {
		case 'o':
			args.f_out = optarg;
			break;

		case 'i':
			args.vol_index = atoi(optarg);
			if (args.vol_index < 0 || args.vol_index >= UBI_MAX_VOLUMES)
				return errmsg("bad volume index: \"%s\"", optarg);
			index_set = 1;
			break;

		case 'n':
			args.vol_name = optarg;
			break;

		case 'p':
			args.peb_size = ubiutils_get_bytes(optarg);
			if (args.peb_size <= 0)
				return errmsg("bad physical eraseblock size: \"%s\"", optarg);
			break;

		case 's':
			args.skip_bad = 1;
			break;

		case 'v':
			args.verbose = 1;
			break;

		case 'h':
			printf("%s\n", doc);
			printf("%s\n\n", usage);
			printf("%s\n", optionsstr);
			exit(EXIT_SUCCESS);

		case 'V':
			printf("%s version %s\n", PROGRAM_NAME, VERSION);
			exit(EXIT_SUCCESS);

		default:
			fprintf(stderr, "Use -h for help\n");
			return -1;
		}
	}

	if (optind == argc)
		return errmsg("input UBI file was not specified (use -h for help)");

	if (optind != argc - 1)
		return errmsg("more than one UBI file was specified (use -h for help)");

	args.f_in = argv[optind];

	if (args.peb_size < 0)
		return errmsg("physical eraseblock size was not specified (use -h for help)");

	if (!index_set && !args.vol_name)
		return errmsg("UBI volume not specified (use -h for help)");

	if (index_set && args.vol_name)
		return errmsg("UBI volume specified by both name and index (use -h for help)");

	if (!args.f_out)
		return errmsg("output file was not specified (use -h for help)");

	return 0;
}

static int read_headers(int fd, struct img_info *imi)
{
	struct ubi_ec_hdr ec_hdr;
	struct ubi_vid_hdr vid_hdr;
	__be32 magic, crc;
	off_t seek;

	if (read(fd, &ec_hdr, sizeof(ec_hdr)) != sizeof(ec_hdr))
		return sys_errmsg("failed to read EC header");

	magic = be32_to_cpu(ec_hdr.magic);
	if (magic != UBI_EC_HDR_MAGIC)
		return errmsg("bad magic of EC header");

	crc = mtd_crc32(UBI_CRC32_INIT, &ec_hdr, UBI_EC_HDR_SIZE_CRC);
	if (be32_to_cpu(ec_hdr.hdr_crc) != crc)
		return errmsg("bad CRC of EC header");

	imi->vid_hdr_offset = be32_to_cpu(ec_hdr.vid_hdr_offset);
	imi->data_offset = be32_to_cpu(ec_hdr.data_offset);

	DEBUG(("vid_hdr_offset=%u, data_offset=%u\n",
		imi->vid_hdr_offset, imi->data_offset));

	if (imi->data_offset >= (unsigned)args.peb_size)
		return errmsg("data_offset >= peb_size");

	seek = (off_t)(imi->vid_hdr_offset - sizeof(ec_hdr));
	if (lseek(fd, seek, SEEK_CUR) == -1)
		return sys_errmsg("cannot seek input file");

	if (read(fd, &vid_hdr, sizeof(vid_hdr)) != sizeof(vid_hdr))
		return sys_errmsg("failed to read vid header");

	magic = be32_to_cpu(vid_hdr.magic);
	if (magic == 0xfffffffful) {

		DEBUG(("empty eraseblock\n"));
		imi->vol_id = magic;
		imi->lnum = magic;
		return 0;

	} else if (magic != UBI_VID_HDR_MAGIC)
		return errmsg("bad magic of vid header");

	crc = mtd_crc32(UBI_CRC32_INIT, &vid_hdr, UBI_VID_HDR_SIZE_CRC);
	if (be32_to_cpu(vid_hdr.hdr_crc) != crc)
		return errmsg("bad CRC of vid header");

	imi->vol_id = be32_to_cpu(vid_hdr.vol_id);
	imi->lnum = be32_to_cpu(vid_hdr.lnum);

	DEBUG(("vol_id=0x%08x, lnum=%u\n", imi->vol_id, imi->lnum));

	seek = (off_t)(imi->data_offset - (imi->vid_hdr_offset + sizeof(vid_hdr)));
	if (lseek(fd, seek, SEEK_CUR) == -1)
		return sys_errmsg("cannot seek input file");

	return 0;
}

static int read_ubi_info(int in_fd, struct img_info *imi)
{
	struct ubi_vtbl_record vtbl_rec;
	off_t seek = 0;
	unsigned vol_ix;
	__be32 crc;

	for (;;) {

		if (lseek(in_fd, seek, SEEK_SET) == -1)
			return sys_errmsg("cannot seek input file");

		if (read_headers(in_fd, imi))
			return -1;

		if (imi->vol_id == UBI_LAYOUT_VOLUME_ID)
			break;

		seek += args.peb_size;
		if (seek >= imi->size)
			return errmsg("volume table EB not found");
	}

	if (args.vol_name) {

		vol_ix = 0;
		while (vol_ix < UBI_MAX_VOLUMES) {

			if (read(in_fd, &vtbl_rec, sizeof(vtbl_rec)) != sizeof(vtbl_rec))
				return sys_errmsg("failed to read vtbl record");

			crc = mtd_crc32(UBI_CRC32_INIT, &vtbl_rec, UBI_VTBL_RECORD_SIZE_CRC);
			if (be32_to_cpu(vtbl_rec.crc) != crc)
				return errmsg("bad CRC of volume table record");

			if (!strncmp(args.vol_name, (char *)vtbl_rec.name, UBI_VOL_NAME_MAX)) {
				args.vol_index = vol_ix;
				break;
			}

			++vol_ix;
		}

		if (vol_ix >= UBI_MAX_VOLUMES)
			return errmsg("volume '%s' not found", args.vol_name);

	} else {

		seek = (off_t)(args.vol_index * UBI_VTBL_RECORD_SIZE);
		if (lseek(in_fd, seek, SEEK_CUR) == -1)
			return sys_errmsg("cannot seek input file");

		if (read(in_fd, &vtbl_rec, sizeof(vtbl_rec)) != sizeof(vtbl_rec))
			return sys_errmsg("failed to read vtbl record");

		crc = mtd_crc32(UBI_CRC32_INIT, &vtbl_rec, UBI_VTBL_RECORD_SIZE_CRC);
		if (be32_to_cpu(vtbl_rec.crc) != crc)
			return errmsg("bad CRC of volume table record");

		if (!strnlen((char *)vtbl_rec.name, UBI_VOL_NAME_MAX))
			return errmsg("volume #%d does not exist", args.vol_index);

		args.vol_name = strndup((char *)vtbl_rec.name, UBI_VOL_NAME_MAX);
	}

	DEBUG(("vol_name='%s', vol_index=%d\n", args.vol_name, args.vol_index));

	return 0;
}

static int extract_volume_data(int in_fd, int out_fd, struct img_info *imi)
{
	struct img_info local_imi;
	unsigned data_size = args.peb_size - imi->data_offset;
	void * buf;
	off_t r_seek, w_seek;

	buf = malloc(data_size);
	if (!buf)
		return sys_errmsg("failed to allocate buffer");

	for (r_seek = 0; r_seek < imi->size; r_seek += args.peb_size) {

		if (lseek(in_fd, r_seek, SEEK_SET) == -1) {
			sys_errmsg("cannot seek input file");
			goto err_mem;
		}

		if (read_headers(in_fd, &local_imi)) {
			if (args.skip_bad)
				continue;
			else
				goto err_mem;
		}

		if (local_imi.vol_id != (unsigned)args.vol_index)
			continue;

		if (read(in_fd, buf, data_size) != data_size) {
			sys_errmsg("failed to read data");
			goto err_mem;
		}

		w_seek = (off_t)(local_imi.lnum * data_size);
		if (lseek(out_fd, w_seek, SEEK_SET) == -1) {
			sys_errmsg("cannot seek output file");
			goto err_mem;
		}

		if (write(out_fd, buf, data_size) != data_size) {
			sys_errmsg("failed to write data");
			goto err_mem;
		}
	}

	return 0;

  err_mem:
	free(buf);
	return -1;
}

int main(int argc, char * const argv[])
{
	struct stat st;
	struct img_info imi;
	int in_fd, out_fd;
	int err;
	int ret = EXIT_FAILURE;

	err = parse_opt(argc, argv);
	if (err)
		return ret;

	if (stat(args.f_in, &st)) {
		sys_errmsg("cannot stat input file \"%s\"", args.f_in);
		return ret;
	}

	if (!st.st_size || (st.st_size % args.peb_size != 0)) {
		errmsg("bad size of input file (%lu)", st.st_size);
		return ret;
	}

	memset(&imi, 0, sizeof(imi));
	imi.size = st.st_size;

	in_fd = open(args.f_in, O_RDONLY);
	if (in_fd < 0) {
		sys_errmsg("cannot open input file \"%s\"", args.f_in);
		return ret;
	}

	err = read_ubi_info(in_fd, &imi);
	if (err)
		goto err_in;

	verbose(args.verbose, "Volume id:    %d", args.vol_index);
	verbose(args.verbose, "Volume name:  %s", args.vol_name);
	verbose(args.verbose, "PEB size:     %d", args.peb_size);
	verbose(args.verbose, "LEB size:     %u",
		(unsigned)(args.peb_size - imi.data_offset));
	verbose(args.verbose, "VID offset:   %d", imi.vid_hdr_offset);
	verbose(args.verbose, "data offset:  %d", imi.data_offset);

	out_fd = open(args.f_out, O_CREAT | O_TRUNC | O_WRONLY,
				  S_IWUSR | S_IRUSR | S_IRGRP | S_IWGRP | S_IROTH);
	if (out_fd < 0) {
		sys_errmsg("cannot open output file \"%s\"", args.f_out);
		goto err_in;
	}

	err = extract_volume_data(in_fd, out_fd, &imi);
	if (!err)
		ret = EXIT_SUCCESS;

	close(out_fd);
  err_in:
	close(in_fd);

	return ret;
}

