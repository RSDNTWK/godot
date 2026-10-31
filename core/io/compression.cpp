/**************************************************************************/
/*  compression.cpp                                                       */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "compression.h"

#include "core/io/pck_lzma.h"
#include "core/io/zip_io.h"

#include <thirdparty/misc/fastlz.h>

#include <zstd.h>

#include <climits>
#include <cstring>

#ifdef BROTLI_ENABLED
#include <brotli/decode.h>
#endif

// Caches for zstd.
static BinaryMutex mutex;
static ZSTD_DCtx *current_zstd_d_ctx = nullptr;
static bool current_zstd_long_distance_matching;
static int current_zstd_window_log_size;

static _FORCE_INLINE_ PCKLzmaOptions _get_default_lzma2_options() {
	PCKLzmaOptions options;
	options.compression_level = 9;
	options.dictionary_size_mb = 64;
	options.word_size = 64;
	options.threads = 1;
	options.memory_usage_percent = 80;
	return options;
}

int64_t Compression::compress(uint8_t *p_dst, const uint8_t *p_src, int64_t p_src_size, Mode p_mode) {
	ERR_FAIL_COND_V(p_src_size < 0 || uint64_t(p_src_size) > SIZE_MAX, -1);
	ERR_FAIL_COND_V(!p_src && p_src_size > 0, -1);
	ERR_FAIL_COND_V(!p_dst && (p_src_size > 0 || p_mode != MODE_LZMA2), -1);
	switch (p_mode) {
		case MODE_BROTLI: {
			ERR_FAIL_V_MSG(-1, "Only brotli decompression is supported.");
		} break;
		case MODE_FASTLZ: {
			ERR_FAIL_COND_V_MSG(p_src_size > INT32_MAX, -1, "Cannot compress a FastLZ/LZ77 file 2 GiB or larger. LZ77 supports larger files, but FastLZ's implementation uses C++ `int` so is limited to 2 GiB. Consider using Zstd instead.");
			if (p_src_size < 16) {
				uint8_t src[16];
				memset(&src[p_src_size], 0, 16 - p_src_size);
				if (p_src_size > 0) {
					memcpy(src, p_src, p_src_size);
				}
				return fastlz_compress(src, 16, p_dst);
			} else {
				return fastlz_compress(p_src, p_src_size, p_dst);
			}

		} break;
		case MODE_DEFLATE:
		case MODE_GZIP: {
			ERR_FAIL_COND_V_MSG(p_src_size > INT32_MAX, -1, "Cannot compress a Deflate or GZip file 2 GiB or larger. Deflate and GZip are both limited to 4 GiB, and ZLib's implementation uses C++ `int` so is limited to 2 GiB. Consider using Zstd instead.");
			int window_bits = p_mode == MODE_DEFLATE ? 15 : 15 + 16;

			z_stream strm;
			strm.zalloc = zipio_alloc;
			strm.zfree = zipio_free;
			strm.opaque = Z_NULL;
			int level = p_mode == MODE_DEFLATE ? zlib_level : gzip_level;
			int err = deflateInit2(&strm, level, Z_DEFLATED, window_bits, 8, Z_DEFAULT_STRATEGY);
			if (err != Z_OK) {
				return -1;
			}

			strm.avail_in = p_src_size;
			int aout = deflateBound(&strm, p_src_size);
			strm.avail_out = aout;
			strm.next_in = (Bytef *)p_src;
			strm.next_out = p_dst;
			deflate(&strm, Z_FINISH);
			aout = aout - strm.avail_out;
			deflateEnd(&strm);
			return aout;

		} break;
		case MODE_ZSTD: {
			ZSTD_CCtx *cctx = ZSTD_createCCtx();
			ERR_FAIL_NULL_V(cctx, -1);
			ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, zstd_level);
			if (zstd_long_distance_matching) {
				ZSTD_CCtx_setParameter(cctx, ZSTD_c_enableLongDistanceMatching, 1);
				ZSTD_CCtx_setParameter(cctx, ZSTD_c_windowLog, zstd_window_log_size);
			}
			const int64_t max_dst_size = get_max_compressed_buffer_size(p_src_size, MODE_ZSTD);
			if (max_dst_size < 0) {
				ZSTD_freeCCtx(cctx);
				return -1;
			}
			const size_t ret = ZSTD_compressCCtx(cctx, p_dst, max_dst_size, p_src, p_src_size, zstd_level);
			ZSTD_freeCCtx(cctx);
			ERR_FAIL_COND_V(ZSTD_isError(ret), -1);
			return (int64_t)ret;
		} break;
		case MODE_LZMA2: {
			ERR_FAIL_COND_V_MSG(p_src_size > INT32_MAX, -1, "Cannot compress an LZMA2 stream 2 GiB or larger.");
			if (p_src_size == 0) {
				return 0;
			}
			Vector<uint8_t> src_data;
			ERR_FAIL_COND_V(src_data.resize(p_src_size) != OK, -1);
			if (p_src_size > 0) {
				memcpy(src_data.ptrw(), p_src, p_src_size);
			}

			Vector<uint8_t> compressed_data;
			const Error err = compress_lzma2(src_data, compressed_data, _get_default_lzma2_options());
			ERR_FAIL_COND_V_MSG(err != OK, -1, "Failed to compress LZMA2 stream.");
			if (!compressed_data.is_empty()) {
				memcpy(p_dst, compressed_data.ptr(), compressed_data.size());
			}
			return compressed_data.size();
		} break;
	}

	ERR_FAIL_V(-1);
}

int64_t Compression::get_max_compressed_buffer_size(int64_t p_src_size, Mode p_mode) {
	ERR_FAIL_COND_V(p_src_size < 0 || uint64_t(p_src_size) > SIZE_MAX, -1);
	switch (p_mode) {
		case MODE_BROTLI: {
			ERR_FAIL_V_MSG(-1, "Only brotli decompression is supported.");
		} break;
		case MODE_FASTLZ: {
			ERR_FAIL_COND_V_MSG(p_src_size > INT32_MAX, -1, "Cannot compress a FastLZ/LZ77 file 2 GiB or larger. LZ77 supports larger files, but FastLZ's implementation uses C++ `int` so is limited to 2 GiB. Consider using Zstd instead.");
			int64_t ss = p_src_size + p_src_size * 6 / 100;
			if (ss < 66) {
				ss = 66;
			}
			return ss;

		} break;
		case MODE_DEFLATE:
		case MODE_GZIP: {
			ERR_FAIL_COND_V_MSG(p_src_size > INT32_MAX, -1, "Cannot compress a Deflate or GZip file 2 GiB or larger. Deflate and GZip are both limited to 4 GiB, and ZLib's implementation uses C++ `int` so is limited to 2 GiB. Consider using Zstd instead.");
			int window_bits = p_mode == MODE_DEFLATE ? 15 : 15 + 16;

			z_stream strm;
			strm.zalloc = zipio_alloc;
			strm.zfree = zipio_free;
			strm.opaque = Z_NULL;
			int err = deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, window_bits, 8, Z_DEFAULT_STRATEGY);
			if (err != Z_OK) {
				return -1;
			}
			int aout = deflateBound(&strm, p_src_size);
			deflateEnd(&strm);
			return aout;
		} break;
		case MODE_ZSTD: {
			const size_t bound = ZSTD_compressBound(p_src_size);
			ERR_FAIL_COND_V(ZSTD_isError(bound) || bound == 0, -1);
			return bound;
		} break;
		case MODE_LZMA2: {
			ERR_FAIL_COND_V_MSG(p_src_size < 0, -1, "Invalid source size.");
			ERR_FAIL_COND_V_MSG(p_src_size > (INT64_MAX - 1024) / 2, -1, "LZMA2 source size is too large.");
			// Conservative buffer to avoid overflow in callers that preallocate.
			return (p_src_size * 2) + 1024;
		} break;
	}

	ERR_FAIL_V(-1);
}

int64_t Compression::decompress(uint8_t *p_dst, int64_t p_dst_max_size, const uint8_t *p_src, int64_t p_src_size, Mode p_mode) {
	ERR_FAIL_COND_V(p_src_size < 0 || p_dst_max_size < 0 || uint64_t(p_src_size) > SIZE_MAX || uint64_t(p_dst_max_size) > SIZE_MAX, -1);
	ERR_FAIL_COND_V((!p_src && p_src_size > 0) || (!p_dst && p_dst_max_size > 0), -1);
	switch (p_mode) {
		case MODE_BROTLI: {
#ifdef BROTLI_ENABLED
			size_t ret_size = p_dst_max_size;
			BrotliDecoderResult res = BrotliDecoderDecompress(p_src_size, p_src, &ret_size, p_dst);
			ERR_FAIL_COND_V(res != BROTLI_DECODER_RESULT_SUCCESS, -1);
			return ret_size;
#else
			ERR_FAIL_V_MSG(-1, "Godot was compiled without brotli support.");
#endif
		} break;
		case MODE_FASTLZ: {
			ERR_FAIL_COND_V(p_dst_max_size > INT32_MAX || p_src_size <= 0 || p_src_size > INT32_MAX, -1);
			if (p_dst_max_size < 16) {
				uint8_t temporary[16];
				const int decoded = fastlz_decompress(p_src, p_src_size, temporary, sizeof(temporary));
				ERR_FAIL_COND_V(decoded <= 0 || decoded < p_dst_max_size, -1);
				if (p_dst_max_size > 0) {
					memcpy(p_dst, temporary, p_dst_max_size);
				}
				return p_dst_max_size;
			}
			const int decoded = fastlz_decompress(p_src, p_src_size, p_dst, p_dst_max_size);
			return decoded > 0 ? decoded : -1;
		} break;
		case MODE_DEFLATE:
		case MODE_GZIP: {
			ERR_FAIL_COND_V_MSG(p_dst_max_size > INT32_MAX || p_src_size > INT32_MAX, -1, "Cannot decompress a Deflate or GZip file 2 GiB or larger. Deflate and GZip are both limited to 4 GiB, and ZLib's implementation uses C++ `int` so is limited to 2 GiB. Consider using Zstd instead.");
			int window_bits = p_mode == MODE_DEFLATE ? 15 : 15 + 16;

			z_stream strm;
			strm.zalloc = zipio_alloc;
			strm.zfree = zipio_free;
			strm.opaque = Z_NULL;
			strm.avail_in = 0;
			strm.next_in = Z_NULL;
			int err = inflateInit2(&strm, window_bits);
			ERR_FAIL_COND_V(err != Z_OK, -1);

			strm.avail_in = p_src_size;
			uint8_t empty_output;
			strm.avail_out = p_dst_max_size > 0 ? p_dst_max_size : 1;
			strm.next_in = (Bytef *)p_src;
			strm.next_out = p_dst_max_size > 0 ? p_dst : &empty_output;

			err = inflate(&strm, Z_FINISH);
			int total = strm.total_out;
			inflateEnd(&strm);
			ERR_FAIL_COND_V(err != Z_STREAM_END || total > p_dst_max_size, -1);
			return total;
		} break;
		case MODE_ZSTD: {
			MutexLock lock(mutex);

			if (!current_zstd_d_ctx || current_zstd_long_distance_matching != zstd_long_distance_matching || current_zstd_window_log_size != zstd_window_log_size) {
				if (current_zstd_d_ctx) {
					ZSTD_freeDCtx(current_zstd_d_ctx);
				}

				current_zstd_d_ctx = ZSTD_createDCtx();
				ERR_FAIL_NULL_V(current_zstd_d_ctx, -1);
				if (zstd_long_distance_matching) {
					const size_t parameter_result = ZSTD_DCtx_setParameter(current_zstd_d_ctx, ZSTD_d_windowLogMax, zstd_window_log_size);
					if (ZSTD_isError(parameter_result)) {
						ZSTD_freeDCtx(current_zstd_d_ctx);
						current_zstd_d_ctx = nullptr;
						return -1;
					}
				}
				current_zstd_long_distance_matching = zstd_long_distance_matching;
				current_zstd_window_log_size = zstd_window_log_size;
			}

			const size_t ret = ZSTD_decompressDCtx(current_zstd_d_ctx, p_dst, p_dst_max_size, p_src, p_src_size);
			ERR_FAIL_COND_V(ZSTD_isError(ret), -1);
			return (int64_t)ret;
		} break;
		case MODE_LZMA2: {
			if (p_dst_max_size == 0 && p_src_size == 0) {
				return 0;
			}
			Vector<uint8_t> dst_data;
			const Error err = decompress_lzma2(p_src, p_src_size, p_dst_max_size, dst_data);
			ERR_FAIL_COND_V_MSG(err != OK, -1, "Failed to decompress LZMA2 stream.");
			if (!dst_data.is_empty()) {
				memcpy(p_dst, dst_data.ptr(), dst_data.size());
			}
			return dst_data.size();
		} break;
	}

	ERR_FAIL_V(-1);
}

/**
	This will handle both Gzip and Deflate streams. It will automatically allocate the output buffer into the provided p_dst_vect Vector.
	This is required for compressed data whose final uncompressed size is unknown, as is the case for HTTP response bodies.
	This is much slower however than using Compression::decompress because it may result in multiple full copies of the output buffer.
*/
int Compression::decompress_dynamic(Vector<uint8_t> *p_dst_vect, int64_t p_max_dst_size, const uint8_t *p_src, int64_t p_src_size, Mode p_mode) {
	ERR_FAIL_NULL_V(p_dst_vect, Z_STREAM_ERROR);
	p_dst_vect->clear();
	ERR_FAIL_COND_V(!p_src || p_src_size <= 0 || uint64_t(p_src_size) > SIZE_MAX, Z_DATA_ERROR);
	ERR_FAIL_COND_V(gzip_chunk <= 0 || p_max_dst_size < -1, Z_STREAM_ERROR);
	const int64_t limit = p_max_dst_size < 0 ? INT64_MAX : p_max_dst_size;
	int64_t produced = 0;
	uint8_t overflow_byte = 0;
	auto fail = [&](int p_error) {
		p_dst_vect->clear();
		return p_error;
	};
	auto finish = [&]() {
		if (produced == p_dst_vect->size()) {
			return Z_OK;
		}
		// CowData's exclusive shrink treats a failed realloc as fatal. Use a
		// checked allocation so this API can still report finalization failure.
		Vector<uint8_t> compact;
		if (compact.resize(produced) != OK) {
			return fail(Z_MEM_ERROR);
		}
		if (produced) {
			memcpy(compact.ptrw(), p_dst_vect->ptr(), produced);
		}
		*p_dst_vect = std::move(compact);
		return Z_OK;
	};
	auto output_buffer = [&](uint8_t *&r_pointer, size_t &r_available) {
		if (produced == limit) {
			// A stream may still need to consume its trailer at the exact limit.
			r_pointer = &overflow_byte;
			r_available = 1;
			return OK;
		}
		if (produced == p_dst_vect->size()) {
			const int64_t amount = MIN(int64_t(gzip_chunk), limit - produced);
			const Error allocation = p_dst_vect->resize(produced + amount);
			if (allocation != OK) {
				return allocation;
			}
		}
		r_pointer = p_dst_vect->ptrw() + produced;
		r_available = size_t(p_dst_vect->size() - produced);
		return OK;
	};

	if (p_mode == MODE_BROTLI) {
#ifdef BROTLI_ENABLED
		BrotliDecoderState *state = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
		ERR_FAIL_NULL_V(state, Z_MEM_ERROR);
		struct DecoderCleanup {
			BrotliDecoderState *state;
			~DecoderCleanup() { BrotliDecoderDestroyInstance(state); }
		} cleanup{ state };
		const uint8_t *next_in = p_src;
		size_t avail_in = p_src_size;
		size_t total_out = 0;
		while (true) {
			uint8_t *next_out = nullptr;
			size_t avail_out = 0;
			if (output_buffer(next_out, avail_out) != OK) {
				return fail(Z_MEM_ERROR);
			}
			const size_t before_in = avail_in;
			const size_t before_out = avail_out;
			const bool probing = produced == limit;
			const BrotliDecoderResult ret = BrotliDecoderDecompressStream(state, &avail_in, &next_in, &avail_out, &next_out, &total_out);
			const size_t amount = before_out - avail_out;
			if (probing && amount != 0) {
				return fail(Z_BUF_ERROR);
			}
			produced += amount;
			if (ret == BROTLI_DECODER_RESULT_ERROR) {
				WARN_PRINT(BrotliDecoderErrorString(BrotliDecoderGetErrorCode(state)));
				return fail(Z_DATA_ERROR);
			}
			if (ret == BROTLI_DECODER_RESULT_SUCCESS) {
				return finish();
			}
			if ((ret == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT && avail_in == 0) || (before_in == avail_in && amount == 0)) {
				return fail(Z_DATA_ERROR);
			}
		}
#else
		ERR_FAIL_V_MSG(Z_ERRNO, "Godot was compiled without brotli support.");
#endif
	} else {
		// This function only supports GZip and Deflate.
		ERR_FAIL_COND_V_MSG(p_mode != MODE_DEFLATE && p_mode != MODE_GZIP, Z_ERRNO, "Dynamic decompression is only supported with gzip, DEFLATE, and Brotli compression methods.");

		z_stream strm{};
		int window_bits = p_mode == MODE_DEFLATE ? 15 : 15 + 16;
		const int initialized = inflateInit2(&strm, window_bits);
		ERR_FAIL_COND_V(initialized != Z_OK, initialized);
		struct InflateCleanup {
			z_stream *stream;
			~InflateCleanup() { inflateEnd(stream); }
		} cleanup{ &strm };
		int64_t supplied = 0;
		while (true) {
			if (strm.avail_in == 0 && supplied < p_src_size) {
				const uInt amount = uInt(MIN(uint64_t(p_src_size - supplied), uint64_t(UINT_MAX)));
				strm.next_in = const_cast<Bytef *>(p_src + supplied);
				strm.avail_in = amount;
				supplied += amount;
			}
			uint8_t *next_out = nullptr;
			size_t available = 0;
			if (output_buffer(next_out, available) != OK) {
				return fail(Z_MEM_ERROR);
			}
			strm.next_out = next_out;
			strm.avail_out = uInt(available);
			const uInt before_in = strm.avail_in;
			const uInt before_out = strm.avail_out;
			const bool probing = produced == limit;
			const int ret = inflate(&strm, Z_SYNC_FLUSH);
			const uInt amount = before_out - strm.avail_out;
			if (probing && amount != 0) {
				return fail(Z_BUF_ERROR);
			}
			produced += amount;
			if (ret == Z_STREAM_END) {
				return finish();
			}
			if (ret != Z_OK || (before_in == strm.avail_in && amount == 0)) {
				return fail(ret == Z_NEED_DICT || ret == Z_BUF_ERROR || ret == Z_OK ? Z_DATA_ERROR : ret);
			}
		}
	}
}
