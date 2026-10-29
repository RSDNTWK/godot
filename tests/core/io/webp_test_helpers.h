#pragma once
#include "core/io/image.h"
namespace TestWebPSafety {
Vector<uint8_t> make_static(bool p_alpha = false);
Vector<uint8_t> make_animation(int p_frames, int p_loops = 0, bool p_composite = false);
struct Fixture {
	String path;
	explicit Fixture(const Vector<uint8_t> &p_bytes);
	~Fixture();
};
}
