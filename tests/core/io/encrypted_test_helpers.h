#pragma once
#include "core/templates/vector.h"
namespace TestEncryptedSafety {
Vector<uint8_t> key();
Vector<uint8_t> stream(int p_size, bool p_magic = true, bool p_big_endian = false, int p_prefix = 0);
}
