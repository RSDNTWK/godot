/**************************************************************************/
/*  astcenc_godot.h                                                       */
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

#ifndef ASTCENC_GODOT_H
#define ASTCENC_GODOT_H

// Load the upstream library's system headers in the global namespace before
// compiling its implementation inside our private namespace. Keep this list in
// sync with astcenc's includes when updating the dependency.
#include <algorithm>
#include <array>
#include <assert.h>
#include <atomic>
#include <cassert>
#include <cfenv>
#include <cfloat>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <new>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(__SSE__) || defined(__SSE2__) || defined(_M_IX86) || defined(_M_AMD64) || (defined(ASTCENC_SSE) && ASTCENC_SSE != 0) || (defined(ASTCENC_AVX) && ASTCENC_AVX != 0)
#include <immintrin.h>
#endif
#if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(_M_ARM64) || (defined(ASTCENC_NEON) && ASTCENC_NEON != 0)
#include <arm_neon.h>
#endif
#if defined(__ARM_FEATURE_SVE) || (defined(ASTCENC_SVE) && ASTCENC_SVE != 0)
#include <arm_sve.h>
#include <arm_neon_sve_bridge.h>
#endif
#if defined(__riscv_v_fixed_vlen) && __riscv_v_fixed_vlen == 256
#include <riscv_vector.h>
#endif

// Namespace isolation also needs distinct C-linkage API names. ANGLE can link
// another version of astcenc into the same executable.
#define astcenc_config_init godot_astcenc_config_init
#define astcenc_context_alloc godot_astcenc_context_alloc
#define astcenc_compress_image godot_astcenc_compress_image
#define astcenc_compress_reset godot_astcenc_compress_reset
#define astcenc_compress_cancel godot_astcenc_compress_cancel
#define astcenc_decompress_image godot_astcenc_decompress_image
#define astcenc_decompress_reset godot_astcenc_decompress_reset
#define astcenc_context_free godot_astcenc_context_free
#define astcenc_get_block_info godot_astcenc_get_block_info
#define astcenc_get_error_string godot_astcenc_get_error_string

namespace godot_astcenc {
using std::abs;
#include <astcenc.h>
}

#endif // ASTCENC_GODOT_H
