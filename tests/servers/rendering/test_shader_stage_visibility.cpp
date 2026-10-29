/**************************************************************************/
/*  test_shader_stage_visibility.cpp                                    */
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

TEST_FORCE_LINK(test_shader_stage_visibility)

#include "servers/rendering/rendering_shader_container.h"
#include "tests/servers/rendering/shader_stage_fixtures.h"

#include <cstring>

namespace TestShaderStageVisibility {

using RDC = RenderingDeviceCommons;

class Container : public RenderingShaderContainer {
public:
	void use_legacy_ray_visibility() {
		for (ReflectionBindingData &binding : reflection_binding_set_uniforms_data) {
			binding.stages = 0x0f;
		}
		reflection_data.push_constant_stages_mask = 0x0f;
	}

protected:
	uint32_t _format() const override { return 0xf021; }
	uint32_t _format_version() const override { return 1; }
	bool _set_code_from_spirv(const ReflectShader &p_shader) override {
		shaders.clear();
		for (const ReflectShaderStage &stage : p_shader.shader_stages) {
			Shader shader;
			shader.shader_stage = stage.shader_stage;
			shader.code_compressed_bytes = stage.spirv_data();
			shader.code_decompressed_size = shader.code_compressed_bytes.size();
			shaders.push_back(shader);
		}
		return true;
	}
};

struct Fixture {
	RDC::ShaderStage stage;
	const uint32_t *words;
	size_t word_count;
};

#define STAGE_FIXTURE(stage, name) { RDC::SHADER_STAGE_##stage, TestShaderStageData::name, sizeof(TestShaderStageData::name) / sizeof(uint32_t) }
static const Fixture fixtures[] = {
	STAGE_FIXTURE(VERTEX, vert),
	STAGE_FIXTURE(FRAGMENT, frag),
	STAGE_FIXTURE(COMPUTE, comp),
	STAGE_FIXTURE(RAYGEN, rgen),
	STAGE_FIXTURE(ANY_HIT, rahit),
	STAGE_FIXTURE(CLOSEST_HIT, rchit),
	STAGE_FIXTURE(MISS, rmiss),
	STAGE_FIXTURE(INTERSECTION, rint),
};
#undef STAGE_FIXTURE

static RDC::ShaderStageSPIRVData shader(const Fixture &p_fixture) {
	RDC::ShaderStageSPIRVData result;
	result.shader_stage = p_fixture.stage;
	REQUIRE(result.spirv.resize(p_fixture.word_count * sizeof(uint32_t)) == OK);
	memcpy(result.spirv.ptrw(), p_fixture.words, result.spirv.size());
	return result;
}

static uint64_t ray_mask() {
	uint64_t result = 0;
	for (int i = 3; i < 8; i++) {
		result |= uint64_t(1) << fixtures[i].stage;
	}
	return result;
}

static void check_resources(const RDC::ShaderReflection &p_reflection, uint64_t p_visibility, uint64_t p_submitted) {
	REQUIRE(p_reflection.uniform_sets.size() == 1);
	REQUIRE(p_reflection.uniform_sets[0].size() == 2);
	CHECK(p_reflection.uniform_sets[0][0].binding == 0);
	CHECK(p_reflection.uniform_sets[0][0].type == RDC::UNIFORM_TYPE_UNIFORM_BUFFER);
	CHECK(p_reflection.uniform_sets[0][1].binding == 1);
	CHECK(p_reflection.uniform_sets[0][1].type == RDC::UNIFORM_TYPE_STORAGE_BUFFER);
	for (const RDC::ShaderUniform &uniform : p_reflection.uniform_sets[0]) {
		CHECK(uint64_t(uniform.stages) == p_visibility);
	}
	CHECK(p_reflection.push_constant_size == 16);
	CHECK(uint64_t(p_reflection.push_constant_stages) == p_visibility);
	CHECK(uint64_t(p_reflection.stages_bits) == p_submitted);
	uint64_t submitted = 0;
	for (RDC::ShaderStage stage : p_reflection.stages_vector) {
		submitted |= uint64_t(1) << stage;
	}
	CHECK(submitted == p_submitted);
}

TEST_CASE("[F21][ShaderReflection] Every ray stage widens resource visibility without inventing submitted stages") {
	for (int i = 3; i < 8; i++) {
		Container container;
		RDC::ShaderStageSPIRVData input[] = { shader(fixtures[i]) };
		REQUIRE(container.set_code_from_spirv("single ray stage", input));
		const RDC::ShaderReflection reflection = container.get_shader_reflection();
		CHECK(reflection.pipeline_type == RDC::PIPELINE_TYPE_RAYTRACING);
		check_resources(reflection, ray_mask(), uint64_t(1) << fixtures[i].stage);
	}
}

TEST_CASE("[F21][ShaderReflection] Shared ray resources merge with all and only ray visibility bits") {
	Container container;
	Vector<RDC::ShaderStageSPIRVData> input;
	for (int i = 3; i < 8; i++) {
		input.push_back(shader(fixtures[i]));
	}
	REQUIRE(container.set_code_from_spirv("merged ray stages", input));
	check_resources(container.get_shader_reflection(), ray_mask(), ray_mask());
}

TEST_CASE("[F21][ShaderReflection] Raster and compute resource visibility remains restricted to submitted stages") {
	{
		Container container;
		RDC::ShaderStageSPIRVData input[] = { shader(fixtures[0]), shader(fixtures[1]) };
		REQUIRE(container.set_code_from_spirv("raster stages", input));
		const uint64_t mask = (uint64_t(1) << RDC::SHADER_STAGE_VERTEX) | (uint64_t(1) << RDC::SHADER_STAGE_FRAGMENT);
		const RDC::ShaderReflection reflection = container.get_shader_reflection();
		CHECK(reflection.pipeline_type == RDC::PIPELINE_TYPE_RASTERIZATION);
		check_resources(reflection, mask, mask);
	}
	{
		Container container;
		RDC::ShaderStageSPIRVData input[] = { shader(fixtures[2]) };
		REQUIRE(container.set_code_from_spirv("compute stage", input));
		const uint64_t mask = uint64_t(1) << RDC::SHADER_STAGE_COMPUTE;
		const RDC::ShaderReflection reflection = container.get_shader_reflection();
		CHECK(reflection.pipeline_type == RDC::PIPELINE_TYPE_COMPUTE);
		check_resources(reflection, mask, mask);
		CHECK(reflection.compute_local_size[0] == 1);
		CHECK(reflection.compute_local_size[1] == 1);
		CHECK(reflection.compute_local_size[2] == 1);
	}
}

TEST_CASE("[F21][ShaderReflection] Duplicate and mixed pipeline stage submissions remain rejected") {
	const bool previous = CoreGlobals::print_error_enabled;
	CoreGlobals::print_error_enabled = false;
	{
		Container container;
		RDC::ShaderStageSPIRVData input[] = { shader(fixtures[3]), shader(fixtures[3]) };
		CHECK_FALSE(container.set_code_from_spirv("duplicate ray stage", input));
	}
	{
		Container container;
		RDC::ShaderStageSPIRVData input[] = { shader(fixtures[0]), shader(fixtures[3]) };
		CHECK_FALSE(container.set_code_from_spirv("mixed stages", input));
	}
	CoreGlobals::print_error_enabled = previous;
}

TEST_CASE("[F21][ShaderReflection] Existing cached ray masks are repaired without changing the container format") {
	Container original;
	RDC::ShaderStageSPIRVData input[] = { shader(fixtures[3]) };
	REQUIRE(original.set_code_from_spirv("legacy ray cache", input));
	const Vector<uint8_t> corrected = original.to_bytes();
	original.use_legacy_ray_visibility();
	const Vector<uint8_t> legacy = original.to_bytes();
	REQUIRE(legacy != corrected);
	Container restored;
	REQUIRE(restored.from_bytes(legacy));
	check_resources(restored.get_shader_reflection(), ray_mask(), uint64_t(1) << RDC::SHADER_STAGE_RAYGEN);
	CHECK(restored.to_bytes() == corrected);
}

TEST_CASE("[N011][ShaderReflection] Existing four-byte container layout round trips ray and compute reflection") {
	for (bool ray : { false, true }) {
		Container original;
		Vector<RDC::ShaderStageSPIRVData> input;
		if (ray) {
			for (int i = 3; i < 8; i++) {
				input.push_back(shader(fixtures[i]));
			}
		} else {
			input.push_back(shader(fixtures[2]));
		}
		REQUIRE(original.set_code_from_spirv("unaligned reflection layout", input));
		const Vector<uint8_t> serialized = original.to_bytes();
		REQUIRE_FALSE(serialized.is_empty());
		Container restored;
		REQUIRE(restored.from_bytes(serialized));
		const uint64_t mask = ray ? ray_mask() : uint64_t(1) << RDC::SHADER_STAGE_COMPUTE;
		check_resources(restored.get_shader_reflection(), mask, mask);
		REQUIRE(restored.shaders.size() == original.shaders.size());
		for (int i = 0; i < restored.shaders.size(); i++) {
			CHECK(restored.shaders[i].shader_stage == original.shaders[i].shader_stage);
			CHECK(restored.shaders[i].code_compressed_bytes == original.shaders[i].code_compressed_bytes);
		}
		CHECK(restored.to_bytes() == serialized);
	}
}

} // namespace TestShaderStageVisibility
