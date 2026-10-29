#include "tests/test_macros.h"
TEST_FORCE_LINK(test_webp_safety)

#include "modules/modules_enabled.gen.h"
#ifdef MODULE_WEBP_ENABLED
#include "webp_test_helpers.h"
#include "modules/webp/texture_loader_webp.h"
#include "modules/webp/webp_common.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/os.h"
#include "scene/resources/animated_texture.h"
#include "scene/resources/image_texture.h"
#include "tests/test_utils.h"
#include "thirdparty/libwebp/src/webp/mux.h"
#include <cstring>

namespace TestWebPSafety {
static Vector<uint8_t> encoded_color(int p_width, int p_height, uint8_t p_red, uint8_t p_green, uint8_t p_blue, uint8_t p_alpha) {
	Vector<uint8_t> pixels;
	REQUIRE(pixels.resize(p_width * p_height * 4) == OK);
	for (int i = 0; i < p_width * p_height; i++) {
		pixels.write[i * 4] = p_red;
		pixels.write[i * 4 + 1] = p_green;
		pixels.write[i * 4 + 2] = p_blue;
		pixels.write[i * 4 + 3] = p_alpha;
	}
	Ref<Image> image = memnew(Image(p_width, p_height, false, Image::FORMAT_RGBA8, pixels));
	return WebPCommon::_webp_lossless_pack(image);
}

Vector<uint8_t> make_static(bool p_alpha) {
	return encoded_color(17, 19, 32, 64, 96, p_alpha ? 128 : 255);
}

Vector<uint8_t> make_animation(int p_frames, int p_loops, bool p_composite) {
	const int width = p_composite ? 4 : 17;
	const int height = p_composite ? 4 : 19;
	WebPMux *mux = WebPMuxNew();
	REQUIRE(mux != nullptr);
	REQUIRE(WebPMuxSetCanvasSize(mux, width, height) == WEBP_MUX_OK);
	WebPMuxAnimParams parameters = {};
	parameters.loop_count = p_loops;
	REQUIRE(WebPMuxSetAnimationParams(mux, &parameters) == WEBP_MUX_OK);
	const Vector<uint8_t> first = encoded_color(width, height, p_composite ? 255 : 32, p_composite ? 0 : 64, p_composite ? 0 : 96, 255);
	const Vector<uint8_t> second = encoded_color(p_composite ? 2 : width, p_composite ? 2 : height, p_composite ? 0 : 128, p_composite ? 0 : 144, p_composite ? 255 : 160, p_composite ? 128 : 255);
	const Vector<uint8_t> third = p_composite ? encoded_color(2, 2, 0, 255, 0, 255) : Vector<uint8_t>();
	for (int i = 0; i < p_frames; i++) {
		const Vector<uint8_t> &encoded = p_composite ? (i == 0 ? first : (i == 1 ? second : third)) : (i % 2 == 0 ? first : second);
		WebPMuxFrameInfo frame = {};
		frame.bitstream.bytes = encoded.ptr();
		frame.bitstream.size = encoded.size();
		frame.id = WEBP_CHUNK_ANMF;
		frame.duration = p_composite ? (i + 1) * 10 : 10 + i;
		frame.dispose_method = p_composite && i == 1 ? WEBP_MUX_DISPOSE_BACKGROUND : WEBP_MUX_DISPOSE_NONE;
		frame.blend_method = p_composite && i == 1 ? WEBP_MUX_BLEND : WEBP_MUX_NO_BLEND;
		if (p_composite && i == 1) { frame.x_offset = 2; }
		if (p_composite && i == 2) { frame.y_offset = 2; }
		REQUIRE(WebPMuxPushFrame(mux, &frame, 1) == WEBP_MUX_OK);
	}
	WebPData assembled = {};
	REQUIRE(WebPMuxAssemble(mux, &assembled) == WEBP_MUX_OK);
	Vector<uint8_t> result;
	REQUIRE(result.resize(assembled.size) == OK);
	memcpy(result.ptrw(), assembled.bytes, assembled.size);
	WebPDataClear(&assembled);
	WebPMuxDelete(mux);
	return result;
}

Fixture::Fixture(const Vector<uint8_t> &p_bytes) {
	path = TestUtils::get_temp_path("webp_contract_" + uitos(OS::get_singleton()->get_ticks_usec()) + ".webp");
	Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
	REQUIRE(file.is_valid());
	REQUIRE(file->store_buffer(p_bytes.ptr(), p_bytes.size()));
}
Fixture::~Fixture() { DirAccess::remove_absolute(path); }

TEST_CASE("[F07][WebP] Static RGB and RGBA decode paths retain exact pixels") {
	for (bool alpha : { false, true }) {
		const Vector<uint8_t> encoded = make_static(alpha);
		Image decoded;
		REQUIRE(WebPCommon::webp_load_image_from_buffer(&decoded, encoded.ptr(), encoded.size()) == OK);
		CHECK(decoded.get_width() == 17);
		CHECK(decoded.get_height() == 19);
		CHECK(decoded.get_format() == (alpha ? Image::FORMAT_RGBA8 : Image::FORMAT_RGB8));
		const Vector<uint8_t> pixels = decoded.get_data();
		const int channels = alpha ? 4 : 3;
		for (int i = 0; i < 17 * 19; i++) {
			CHECK(pixels[i * channels] == 32);
			CHECK(pixels[i * channels + 1] == 64);
			CHECK(pixels[i * channels + 2] == 96);
			if (alpha) { CHECK(pixels[i * channels + 3] == 128); }
		}
		Ref<Image> unpacked = WebPCommon::_webp_unpack(encoded);
		REQUIRE(unpacked.is_valid());
		CHECK(unpacked->get_data() == decoded.get_data());
		CHECK(unpacked->get_format() == decoded.get_format());
	}
}

TEST_CASE("[F07][WebP] Truncated RIFF headers are rejected before indexing") {
	const Vector<uint8_t> full = make_static();
	ERR_PRINT_OFF;
	for (int size = 0; size < 12; size++) {
		Vector<uint8_t> short_data = full;
		REQUIRE(short_data.resize(size) == OK);
		CHECK(WebPCommon::_webp_unpack(short_data).is_null());
	}
	ERR_PRINT_ON;
}

TEST_CASE("[SceneTree][F15][WebP] One and256 frames load and257 frames report unavailable") {
	for (int count : { 1, 256, 257 }) {
		const Vector<uint8_t> encoded = make_animation(count);
		Fixture fixture(encoded);
		uint32_t metadata_count = 0;
		REQUIRE(ResourceFormatWebP::get_webp_frame_count(fixture.path, metadata_count) == OK);
		CHECK(metadata_count == uint32_t(count));
		Error error = FAILED;
		Ref<Resource> resource = ResourceFormatWebP::load_texture(fixture.path, &error);
		if (count > AnimatedTexture::MAX_FRAMES) {
			CHECK(resource.is_null());
			CHECK(error == ERR_UNAVAILABLE);
		} else if (count == 1) {
			REQUIRE(resource.is_valid());
			CHECK(error == OK);
			Ref<ImageTexture> texture = resource;
			REQUIRE(texture.is_valid());
			CHECK(texture->get_width() == 17);
			CHECK(texture->get_height() == 19);
		} else {
			CHECK(error == OK);
			Ref<AnimatedTexture> animation = resource;
			REQUIRE(animation.is_valid());
			CHECK(animation->get_frames() == count);
			CHECK_FALSE(animation->get_one_shot());
			REQUIRE(animation->get_frame_texture(count - 1).is_valid());
			CHECK(animation->get_frame_duration(count - 1) == doctest::Approx(0.265));
			Ref<Image> image = animation->get_frame_texture(count - 1)->get_image();
			REQUIRE(image.is_valid());
			CHECK(image->get_pixel(0, 0).is_equal_approx(Color(128 / 255.0f, 144 / 255.0f, 160 / 255.0f, 1)));
		}
	}
}

TEST_CASE("[SceneTree][F15][WebP] Compositing disposal timing and finite loops retain behavior") {
	for (int loops : { 0, 1, 4 }) {
		Fixture fixture(make_animation(3, loops, true));
		Error error = FAILED;
		Ref<AnimatedTexture> animation = ResourceFormatWebP::load_texture(fixture.path, &error);
		REQUIRE(error == OK);
		REQUIRE(animation.is_valid());
		CHECK(animation->get_one_shot() == (loops != 0));
		for (int i = 0; i < 3; i++) {
			CHECK(animation->get_frame_duration(i) == doctest::Approx((i + 1) * 0.01));
		}
		const Color blended = animation->get_frame_texture(1)->get_image()->get_pixel(2, 0);
		CHECK(Math::abs(int(Math::round(blended.r * 255.0f)) - 127) <= 1);
		CHECK(Math::abs(int(Math::round(blended.b * 255.0f)) - 128) <= 1);
		CHECK(blended.a == 1);
		Ref<Image> final = animation->get_frame_texture(2)->get_image();
		CHECK(final->get_pixel(2, 0).a == 0);
		CHECK(final->get_pixel(0, 2).is_equal_approx(Color(0, 1, 0, 1)));
	}
}
} // namespace TestWebPSafety
#endif
