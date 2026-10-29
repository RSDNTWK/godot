/**************************************************************************/
/*  memory.cpp                                                            */
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

#include "memory.h"

#include "core/profiling/profiling.h"
#include "core/templates/safe_refcount.h"

#include <cstdlib>

#ifdef DEBUG_ENABLED
static SafeNumeric<uint64_t> _current_mem_usage;
static SafeNumeric<uint64_t> _max_mem_usage;
#endif

void *operator new(size_t p_size, DefaultAllocator p_allocator) {
	return Memory::alloc_static(p_size);
}

void *operator new(size_t p_size, void *(*p_allocfunc)(size_t p_size)) {
	return p_allocfunc(p_size);
}

void *Memory::alloc_aligned_static(size_t p_bytes, size_t p_alignment) {
	DEV_ASSERT(p_alignment && !(p_alignment & (p_alignment - 1)));
	ERR_FAIL_COND_V(!p_alignment || (p_alignment & (p_alignment - 1)), nullptr);
	ERR_FAIL_COND_V(p_alignment - 1 > SIZE_MAX - sizeof(uint32_t), nullptr);
	const size_t overhead = p_alignment - 1 + sizeof(uint32_t);
	ERR_FAIL_COND_V(p_bytes > SIZE_MAX - overhead, nullptr);

	void *base = malloc(p_bytes + overhead);
	if (base == nullptr) {
		return nullptr;
	}
	void *aligned = (void *)(((uintptr_t)base + sizeof(uint32_t) + p_alignment - 1) & ~(p_alignment - 1));
	const uintptr_t offset = (uintptr_t)aligned - (uintptr_t)base;
	if (offset > UINT32_MAX) {
		free(base);
		return nullptr;
	}
	*((uint32_t *)aligned - 1) = (uint32_t)offset;
	GodotProfileAlloc(base, p_bytes + overhead);
	return aligned;
}

void *Memory::realloc_aligned_static(void *p_memory, size_t p_bytes, size_t p_prev_bytes, size_t p_alignment) {
	if (p_memory == nullptr) {
		return alloc_aligned_static(p_bytes, p_alignment);
	}

	void *ret = alloc_aligned_static(p_bytes, p_alignment);
	if (ret == nullptr) {
		return nullptr;
	}
	memcpy(ret, p_memory, MIN(p_prev_bytes, p_bytes));
	free_aligned_static(p_memory);
	return ret;
}

void Memory::free_aligned_static(void *p_memory) {
	uint32_t offset = *((uint32_t *)p_memory - 1);
	void *p = (void *)((uint8_t *)p_memory - offset);
	GodotProfileFree(p);
	free(p);
}

template <bool p_ensure_zero>
void *Memory::alloc_static(size_t p_bytes, bool p_pad_align) {
#if defined(DEBUG_ENABLED) || defined(GODOT_PROFILER_TRACK_MEMORY)
	bool prepad = true;
#else
	bool prepad = p_pad_align;
#endif

	ERR_FAIL_COND_V(prepad && p_bytes > SIZE_MAX - DATA_OFFSET, nullptr);

	void *mem;
	if constexpr (p_ensure_zero) {
		mem = calloc(1, p_bytes + (prepad ? DATA_OFFSET : 0));
	} else {
		mem = malloc(p_bytes + (prepad ? DATA_OFFSET : 0));
	}

	ERR_FAIL_NULL_V(mem, nullptr);
	GodotProfileAlloc(mem, p_bytes + (prepad ? DATA_OFFSET : 0));

	if (prepad) {
		uint8_t *s8 = (uint8_t *)mem;

		uint64_t *s = (uint64_t *)(s8 + SIZE_OFFSET);
		*s = p_bytes;

#ifdef DEBUG_ENABLED
		uint64_t new_mem_usage = _current_mem_usage.add(p_bytes);
		_max_mem_usage.exchange_if_greater(new_mem_usage);
#endif
		return s8 + DATA_OFFSET;
	} else {
		return mem;
	}
}

template void *Memory::alloc_static<true>(size_t p_bytes, bool p_pad_align);
template void *Memory::alloc_static<false>(size_t p_bytes, bool p_pad_align);

void *Memory::realloc_static(void *p_memory, size_t p_bytes, bool p_pad_align) {
	if (p_memory == nullptr) {
		return alloc_static(p_bytes, p_pad_align);
	}

	uint8_t *mem = (uint8_t *)p_memory;

#if defined(DEBUG_ENABLED) || defined(GODOT_PROFILER_TRACK_MEMORY)
	bool prepad = true;
#else
	bool prepad = p_pad_align;
#endif

	ERR_FAIL_COND_V(prepad && p_bytes > SIZE_MAX - DATA_OFFSET, nullptr);
	if (prepad) {
		mem -= DATA_OFFSET;
		[[maybe_unused]] const uint64_t previous_size = *(uint64_t *)(mem + SIZE_OFFSET);

		if (p_bytes == 0) {
#ifdef DEBUG_ENABLED
			_current_mem_usage.sub(previous_size);
#endif
			GodotProfileFree(mem);
			free(mem);
			return nullptr;
		}

		// A failed realloc retains the allocation and its metadata.
		GodotProfileFree(mem);
		uint8_t *replacement = (uint8_t *)realloc(mem, p_bytes + DATA_OFFSET);
		if (replacement == nullptr) {
			GodotProfileAlloc(mem, previous_size + DATA_OFFSET);
			ERR_FAIL_V(nullptr);
		}
		GodotProfileAlloc(replacement, p_bytes + DATA_OFFSET);
#ifdef DEBUG_ENABLED
		if (p_bytes > previous_size) {
			uint64_t new_mem_usage = _current_mem_usage.add(p_bytes - previous_size);
			_max_mem_usage.exchange_if_greater(new_mem_usage);
		} else {
			_current_mem_usage.sub(previous_size - p_bytes);
		}
#endif
		*(uint64_t *)(replacement + SIZE_OFFSET) = p_bytes;
		return replacement + DATA_OFFSET;
	} else {
		GodotProfileFree(mem);
		mem = (uint8_t *)realloc(mem, p_bytes);

		ERR_FAIL_COND_V(mem == nullptr && p_bytes > 0, nullptr);
		GodotProfileAlloc(mem, p_bytes);

		return mem;
	}
}

void Memory::free_static(void *p_ptr, bool p_pad_align) {
	ERR_FAIL_NULL(p_ptr);

	uint8_t *mem = (uint8_t *)p_ptr;

#if defined(DEBUG_ENABLED) || defined(GODOT_PROFILER_TRACK_MEMORY)
	bool prepad = true;
#else
	bool prepad = p_pad_align;
#endif

	if (prepad) {
		mem -= DATA_OFFSET;

#ifdef DEBUG_ENABLED
		uint64_t *s = (uint64_t *)(mem + SIZE_OFFSET);
		_current_mem_usage.sub(*s);
#endif

		GodotProfileFree(mem);
		free(mem);
	} else {
		GodotProfileFree(mem);
		free(mem);
	}
}

uint64_t Memory::get_mem_available() {
	return -1; // 0xFFFF...
}

uint64_t Memory::get_mem_usage() {
#ifdef DEBUG_ENABLED
	return _current_mem_usage.get();
#else
	return 0;
#endif
}

uint64_t Memory::get_mem_max_usage() {
#ifdef DEBUG_ENABLED
	return _max_mem_usage.get();
#else
	return 0;
#endif
}

_GlobalNil::_GlobalNil() {
	left = this;
	right = this;
	parent = this;
}

_GlobalNil _GlobalNilClass::_nil;
