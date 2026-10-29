#include "tests/test_macros.h"
TEST_FORCE_LINK(test_raytracing_index_buffers)

#ifdef VULKAN_ENABLED

#include "drivers/vulkan/rendering_context_driver_vulkan.h"
#include "servers/rendering/rendering_device.h"

#include <cstring>

namespace TestRaytracingIndexBuffers {

struct Device {
	RenderingContextDriverVulkan context;
	RenderingDevice *rd = nullptr;
	Vector<RID> resources;

	Error initialize() {
		Error error = context.initialize();
		if (error != OK) {
			return error;
		}
		rd = memnew(RenderingDevice);
		return rd->initialize(&context);
	}

	RID keep(RID p_resource) {
		REQUIRE(p_resource.is_valid());
		resources.push_back(p_resource);
		return p_resource;
	}

	~Device() {
		if (rd) {
			for (int i = resources.size() - 1; i >= 0; i--) {
				rd->free_rid(resources[i]);
			}
			memdelete(rd);
		}
	}
};

template <typename T, size_t N>
static Vector<uint8_t> bytes(const T (&p_values)[N]) {
	Vector<uint8_t> result;
	REQUIRE(result.resize(sizeof(p_values)) == OK);
	memcpy(result.ptrw(), p_values, sizeof(p_values));
	return result;
}

static RenderingDevice::AccelerationStructureGeometry triangle(Device &p_device) {
	using RD = RenderingDevice;
	const float vertices[] = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
	const Vector<uint8_t> data = bytes(vertices);
	BitField<RD::BufferCreationBits> flags = RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT;
	flags.set_flag(RD::BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT);
	RD::AccelerationStructureGeometry geometry;
	geometry.vertex_buffer = p_device.keep(p_device.rd->vertex_buffer_create(data.size(), data, flags));
	geometry.vertex_stride = 3 * sizeof(float);
	geometry.vertex_count = 3;
	geometry.vertex_format = RD::DATA_FORMAT_R32G32B32_SFLOAT;
	return geometry;
}

static void build(Device &p_device, RenderingDevice::AccelerationStructureGeometry &p_geometry) {
	RID blas = p_device.keep(p_device.rd->blas_create(Span<RenderingDevice::AccelerationStructureGeometry>(&p_geometry, 1), 0));
	REQUIRE(p_device.rd->blas_build(blas) == OK);
	p_device.rd->submit();
	p_device.rd->sync();
}

// These cases require physical Vulkan ray-query/raytracing hardware. Select
// [F20] with --no-skip on the capable host; ordinary CPU-only test runs skip them.
TEST_CASE_PENDING("[F20][RenderingDevice] Initialized indexed triangles build with known or unknown maxima") {
	using RD = RenderingDevice;
	Device device;
	REQUIRE(device.initialize() == OK);
	REQUIRE((device.rd->has_feature(RD::SUPPORTS_RAYTRACING_PIPELINE) || device.rd->has_feature(RD::SUPPORTS_RAY_QUERY)));

	RD::AccelerationStructureGeometry nonindexed = triangle(device);
	build(device, nonindexed);

	for (RD::IndexBufferFormat format : { RD::INDEX_BUFFER_FORMAT_UINT16, RD::INDEX_BUFFER_FORMAT_UINT32 }) {
		RD::AccelerationStructureGeometry geometry = triangle(device);
		const uint16_t indices16[] = { 0, 1, 2, 0, 1, 2 };
		const uint32_t indices32[] = { 0, 1, 2, 0, 1, 2 };
		const Vector<uint8_t> indices = format == RD::INDEX_BUFFER_FORMAT_UINT16 ? bytes(indices16) : bytes(indices32);
		BitField<RD::BufferCreationBits> flags = RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT;
		flags.set_flag(RD::BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT);
		geometry.index_buffer = device.keep(device.rd->index_buffer_create(6, format, indices, false, flags));
		geometry.index_count = 6;
		build(device, geometry);

#ifdef DEBUG_ENABLED
		// A known maximum remains validated; never submit invalid indices to BLAS build.
		const uint16_t invalid16[] = { 0, 1, 3, 0, 1, 2 };
		const uint32_t invalid32[] = { 0, 1, 3, 0, 1, 2 };
		const Vector<uint8_t> invalid = format == RD::INDEX_BUFFER_FORMAT_UINT16 ? bytes(invalid16) : bytes(invalid32);
		geometry.index_buffer = device.keep(device.rd->index_buffer_create(6, format, invalid, false, flags));
		ERR_PRINT_OFF;
		CHECK_FALSE(device.rd->blas_create(Span<RD::AccelerationStructureGeometry>(&geometry, 1), 0).is_valid());
		ERR_PRINT_ON;
#endif
	}
}

TEST_CASE_PENDING("[F20][RenderingDevice] GPU copies populate index buffers without CPU maximum metadata") {
	using RD = RenderingDevice;
	Device device;
	REQUIRE(device.initialize() == OK);
	REQUIRE((device.rd->has_feature(RD::SUPPORTS_RAYTRACING_PIPELINE) || device.rd->has_feature(RD::SUPPORTS_RAY_QUERY)));

	for (RD::IndexBufferFormat format : { RD::INDEX_BUFFER_FORMAT_UINT16, RD::INDEX_BUFFER_FORMAT_UINT32 }) {
		RD::AccelerationStructureGeometry geometry = triangle(device);
		const uint16_t indices16[] = { 0, 1, 2, 0, 1, 2 };
		const uint32_t indices32[] = { 0, 1, 2, 0, 1, 2 };
		const Vector<uint8_t> indices = format == RD::INDEX_BUFFER_FORMAT_UINT16 ? bytes(indices16) : bytes(indices32);
		BitField<RD::BufferCreationBits> flags = RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT;
		flags.set_flag(RD::BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT);
		geometry.index_buffer = device.keep(device.rd->index_buffer_create(6, format, {}, false, flags));
		geometry.index_count = 6;
		RID source = device.keep(device.rd->storage_buffer_create(indices.size(), indices));
		REQUIRE(device.rd->buffer_copy(source, geometry.index_buffer, 0, 0, indices.size()) == OK);
		device.rd->submit();
		device.rd->sync();
		CHECK(device.rd->buffer_get_data(geometry.index_buffer) == indices);
		build(device, geometry);
	}
}

} // namespace TestRaytracingIndexBuffers

#endif // VULKAN_ENABLED
