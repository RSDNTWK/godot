/**************************************************************************/
/*  test_vulkan_allocation_callbacks.cpp                                  */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_vulkan_allocation_callbacks)

#ifdef VULKAN_ENABLED
#include "drivers/vulkan/rendering_context_driver_vulkan.h"

#include <cstring>

#ifdef VK_TRACK_DRIVER_MEMORY
namespace TestVulkanAllocationCallbacks {

struct ErrorsOff {
	bool previous = CoreGlobals::print_error_enabled;
	ErrorsOff() { CoreGlobals::print_error_enabled = false; }
	~ErrorsOff() { CoreGlobals::print_error_enabled = previous; }
};

// Run with --no-skip --extra-gpu-memory-tracking. No Vulkan device is needed.
TEST_CASE_PENDING("[N015][VulkanAllocation] Null realloc preserves tracked extended object attribution") {
	RenderingContextDriverVulkan context;
	VkAllocationCallbacks *callbacks = context.get_allocation_callbacks(VK_OBJECT_TYPE_SWAPCHAIN_KHR);
	REQUIRE(callbacks != nullptr);
	if (!callbacks) {
		return;
	}
	const uint64_t previous_bytes = context.get_driver_memory_by_object_type(context.VK_TRACKED_OBJECT_TYPE_SWAPCHAIN);
	const uint64_t previous_count = context.get_driver_allocs_by_object_type(context.VK_TRACKED_OBJECT_TYPE_SWAPCHAIN);
	void *data = callbacks->pfnReallocation(callbacks->pUserData, nullptr, 73, 64, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	REQUIRE(data != nullptr);
	if (!data) {
		return;
	}
	CHECK(uintptr_t(data) % 64 == 0);
	CHECK(context.get_driver_memory_by_object_type(context.VK_TRACKED_OBJECT_TYPE_SWAPCHAIN) == previous_bytes + 73);
	CHECK(context.get_driver_allocs_by_object_type(context.VK_TRACKED_OBJECT_TYPE_SWAPCHAIN) == previous_count + 1);
	callbacks->pfnFree(callbacks->pUserData, data);
	CHECK(context.get_driver_memory_by_object_type(context.VK_TRACKED_OBJECT_TYPE_SWAPCHAIN) == previous_bytes);
	CHECK(context.get_driver_allocs_by_object_type(context.VK_TRACKED_OBJECT_TYPE_SWAPCHAIN) == previous_count);
}

TEST_CASE_PENDING("[N015][VulkanAllocation] Realloc shrink failure scope and zero preserve ownership and counters") {
	ErrorsOff quiet;
	RenderingContextDriverVulkan context;
	VkAllocationCallbacks *callbacks = context.get_allocation_callbacks(VK_OBJECT_TYPE_BUFFER);
	REQUIRE(callbacks != nullptr);
	if (!callbacks) {
		return;
	}
	const uint64_t previous_bytes = context.get_driver_total_memory();
	const uint64_t previous_count = context.get_driver_allocation_count();
	CHECK(callbacks->pfnAllocation(callbacks->pUserData, SIZE_MAX, 32, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT) == nullptr);
	CHECK(context.get_driver_total_memory() == previous_bytes);
	CHECK(context.get_driver_allocation_count() == previous_count);
	uint8_t *data = (uint8_t *)callbacks->pfnAllocation(callbacks->pUserData, 257, 64, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	REQUIRE(data != nullptr);
	if (!data) {
		return;
	}
	for (int i = 0; i < 257; i++) {
		data[i] = uint8_t(i);
	}
	CHECK(callbacks->pfnReallocation(callbacks->pUserData, data, SIZE_MAX, 64, VK_SYSTEM_ALLOCATION_SCOPE_CACHE) == nullptr);
	CHECK(context.get_driver_total_memory() == previous_bytes + 257);
	CHECK(context.get_driver_allocation_count() == previous_count + 1);
	CHECK(data[256] == 0);
	uint8_t *shrunk = (uint8_t *)callbacks->pfnReallocation(callbacks->pUserData, data, 13, 64, VK_SYSTEM_ALLOCATION_SCOPE_CACHE);
	REQUIRE(shrunk != nullptr);
	if (!shrunk) {
		callbacks->pfnFree(callbacks->pUserData, data);
		return;
	}
	for (int i = 0; i < 13; i++) {
		CHECK(shrunk[i] == i);
	}
	CHECK(context.get_driver_total_memory() == previous_bytes + 13);
	CHECK(context.get_driver_allocation_count() == previous_count + 1);
	CHECK(callbacks->pfnReallocation(callbacks->pUserData, shrunk, 0, 64, VK_SYSTEM_ALLOCATION_SCOPE_CACHE) == nullptr);
	CHECK(context.get_driver_total_memory() == previous_bytes);
	CHECK(context.get_driver_allocation_count() == previous_count);
	CHECK(callbacks->pfnReallocation(callbacks->pUserData, nullptr, 0, 64, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT) == nullptr);
	CHECK(context.get_driver_allocation_count() == previous_count);
}
} // namespace TestVulkanAllocationCallbacks
#endif
#endif
