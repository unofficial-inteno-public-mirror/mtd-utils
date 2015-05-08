/*
 * Copyright (C) 2008 Nokia Corporation.
 * Copyright (C) 2008 University of Szeged, Hungary
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 *
 * Authors: Artem Bityutskiy
 *          Adrian Hunter
 *          Zoltan Sogor
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <linux/types.h>

#define crc32 __zlib_crc32
#include <zlib.h>
#undef crc32

#include "compr.h"
#include "ubifs-media.h"
#include "mkfs.ubifs.h"

static unsigned long long errcnt = 0;
static struct ubifs_info *c = &info_;

#define DEFLATE_DEF_LEVEL     Z_DEFAULT_COMPRESSION
#define DEFLATE_DEF_WINBITS   11
#define DEFLATE_DEF_MEMLEVEL  8

static int zlib_deflate(void *in_buf, size_t in_len, void *out_buf,
			size_t *out_len)
{
	z_stream strm;

	strm.zalloc = NULL;
	strm.zfree = NULL;

	/*
	 * Match exactly the zlib parameters used by the Linux kernel crypto
	 * API.
	 */
        if (deflateInit2(&strm, DEFLATE_DEF_LEVEL, Z_DEFLATED,
			 -DEFLATE_DEF_WINBITS, DEFLATE_DEF_MEMLEVEL,
			 Z_DEFAULT_STRATEGY)) {
		errcnt += 1;
		return -1;
	}

	strm.next_in = in_buf;
	strm.avail_in = in_len;
	strm.total_in = 0;

	strm.next_out = out_buf;
	strm.avail_out = *out_len;
	strm.total_out = 0;

	if (deflate(&strm, Z_FINISH) != Z_STREAM_END) {
		deflateEnd(&strm);
		errcnt += 1;
		return -1;
	}

	if (deflateEnd(&strm) != Z_OK) {
		errcnt += 1;
		return -1;
	}

	*out_len = strm.total_out;

	return 0;
}

#ifndef WITHOUT_LZO
#include <lzo/lzo1x.h>

static void *lzo_mem;

static int lzo_init(void)
{
	lzo_mem = malloc(LZO1X_999_MEM_COMPRESS);
	if (!lzo_mem)
		return -1;

	return 0;
}

static void lzo_fini(void)
{
	free(lzo_mem);
}

static int lzo_compress(void *in_buf, size_t in_len, void *out_buf,
			size_t *out_len)
{
	lzo_uint len;
	int ret;

	len = *out_len;
	ret = lzo1x_999_compress(in_buf, in_len, out_buf, &len, lzo_mem);
	*out_len = len;

	if (ret != LZO_E_OK) {
		errcnt += 1;
		return -1;
	}

	return 0;
}
#else
static inline int lzo_compress(void *in_buf, size_t in_len, void *out_buf,
			       size_t *out_len) { return -1; }
static inline int lzo_init(void) { return 0; }
static inline void lzo_fini(void) { }
#endif

#ifndef WITHOUT_LZMA

#include <linux/lzma.h>

#define DEBUGPR printf

struct lzma_ctx {
	CLzmaEncHandle *p;
	SizeT propsSize;
	Byte propsEncoded[LZMA_PROPS_SIZE];
} lzma_ctx;

static void lzma_free_workspace(struct lzma_ctx *ctx)
{
	LzmaEnc_Destroy(ctx->p, &lzma_alloc, &lzma_alloc);
}

static int lzma_alloc_workspace(struct lzma_ctx *ctx, CLzmaEncProps *props)
{
	SRes res;

	ctx->p = (CLzmaEncHandle *)LzmaEnc_Create(&lzma_alloc);
	if (ctx->p == NULL)
		return -1;

	res = LzmaEnc_SetProps(ctx->p, props);
	if (res != SZ_OK) {
		DEBUGPR("LzmaEnc_SetProps: res=%d\n", res);
		lzma_free_workspace(ctx);
		return -1;
	}

	ctx->propsSize = sizeof(ctx->propsEncoded);
	res = LzmaEnc_WriteProperties(ctx->p, ctx->propsEncoded, &ctx->propsSize);
	if (res != SZ_OK) {
		DEBUGPR("LzmaEnc_WriteProperties: res=%d\n", res);
		lzma_free_workspace(ctx);
		return -1;
	}

	return 0;
}

static int lzma_init(void)
{
	struct lzma_ctx *ctx = &lzma_ctx;
	int ret;
	CLzmaEncProps props;
	LzmaEncProps_Init(&props);

	props.dictSize = LZMA_BEST_DICT(0x2000);
	props.level = LZMA_BEST_LEVEL;
	props.lc = LZMA_BEST_LC;
	props.lp = LZMA_BEST_LP;
	props.pb = LZMA_BEST_PB;
	props.fb = LZMA_BEST_FB;

	ret = lzma_alloc_workspace(ctx, &props);
	return ret;
}

static void lzma_fini(void)
{
	struct lzma_ctx *ctx = &lzma_ctx;

	lzma_free_workspace(ctx);
}

static int lzma_compress(void *in_buf, size_t in_len, void *out_buf,
			 size_t *out_len)
{
	struct lzma_ctx *ctx = &lzma_ctx;
	SizeT compress_size = (SizeT)(*out_len);
	int ret;

	ret = LzmaEnc_MemEncode(ctx->p, out_buf, &compress_size, in_buf, in_len,
				1, NULL, &lzma_alloc, &lzma_alloc);
	if (ret != SZ_OK) {
		DEBUGPR("LzmaEnc_MemEncode: ret=%d\n", ret);
		return -1;
	}

	*out_len = (unsigned int)compress_size;
	return 0;
}

#else
static inline int lzma_init(void) { return 0; }
static inline void lzma_fini(void) { }
static inline int lzma_compress(void *in_buf, size_t in_len, void *out_buf,
				size_t *out_len) { return -1; }
#endif

static int no_compress(void *in_buf, size_t in_len, void *out_buf,
		       size_t *out_len)
{
	memcpy(out_buf, in_buf, in_len);
	*out_len = in_len;
	return 0;
}

static char *zlib_buf;

static int favor_lzo_compress(void *in_buf, size_t in_len, void *out_buf,
			       size_t *out_len, int *type)
{
	int lzo_ret, zlib_ret;
	size_t lzo_len, zlib_len;

	lzo_len = zlib_len = *out_len;
	lzo_ret = lzo_compress(in_buf, in_len, out_buf, &lzo_len);
	zlib_ret = zlib_deflate(in_buf, in_len, zlib_buf, &zlib_len);
	if (lzo_ret && zlib_ret)
		/* Both compressors failed */
		return -1;

	if (!lzo_ret && !zlib_ret) {
		double percent;

		/* Both compressors succeeded */
		if (lzo_len <= zlib_len )
			goto select_lzo;

		percent = (double)zlib_len / (double)lzo_len;
		percent *= 100;
		if (percent > 100 - c->favor_percent)
			goto select_lzo;
		goto select_zlib;
	}

	if (lzo_ret)
		/* Only zlib compressor succeeded */
		goto select_zlib;

	/* Only LZO compressor succeeded */

select_lzo:
	*out_len = lzo_len;
	*type = MKFS_UBIFS_COMPR_LZO;
	return 0;

select_zlib:
	*out_len = zlib_len;
	*type = MKFS_UBIFS_COMPR_ZLIB;
	memcpy(out_buf, zlib_buf, zlib_len);
	return 0;
}

int compress_data(void *in_buf, size_t in_len, void *out_buf, size_t *out_len,
		  int type)
{
	int ret;

	if (in_len < UBIFS_MIN_COMPR_LEN) {
		no_compress(in_buf, in_len, out_buf, out_len);
		return MKFS_UBIFS_COMPR_NONE;
	}

	if (c->favor_lzo)
		ret = favor_lzo_compress(in_buf, in_len, out_buf, out_len, &type);
	else {
		switch (type) {
		case MKFS_UBIFS_COMPR_LZO:
			ret = lzo_compress(in_buf, in_len, out_buf, out_len);
			break;
		case MKFS_UBIFS_COMPR_ZLIB:
			ret = zlib_deflate(in_buf, in_len, out_buf, out_len);
			break;
		case MKFS_UBIFS_COMPR_LZMA:
			ret = lzma_compress(in_buf, in_len, out_buf, out_len);
			break;
		case MKFS_UBIFS_COMPR_NONE:
			ret = 1;
			break;
		default:
			errcnt += 1;
			ret = 1;
			break;
		}
	}
	if (ret || *out_len >= in_len) {
		no_compress(in_buf, in_len, out_buf, out_len);
		return MKFS_UBIFS_COMPR_NONE;
	}
	return type;
}

int init_compression(void)
{
	int ret;

	ret = lzo_init();
	if (ret)
		goto err;

	zlib_buf = malloc(UBIFS_BLOCK_SIZE * WORST_COMPR_FACTOR);
	if (!zlib_buf)
		goto err_lzo;

	ret = lzma_init();
	if (ret)
		goto err_zlib;

	return 0;

err_zlib:
	free(zlib_buf);
err_lzo:
	lzo_fini();
err:
	return ret;
}

void destroy_compression(void)
{
	lzma_fini();
	free(zlib_buf);
	lzo_fini();
	if (errcnt)
		fprintf(stderr, "%llu compression errors occurred\n", errcnt);
}
