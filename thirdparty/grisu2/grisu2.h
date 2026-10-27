// Godot compatibility formatter. Digit generation uses Dragonbox 1.1.3.
// The formatting helpers below retain the MIT-licensed simdjson/Godot adaptation.
// See LICENSE and ../dragonbox/LICENSE-Boost for the respective licenses.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include <thirdparty/dragonbox/dragonbox.h>

namespace grisu2 {
inline char *append_exponent(char *buf, int e) {

  bool isNegative = e < 0;
  e = isNegative ? -e : e;
  *buf++ = isNegative  ? '-' : '+';

  auto k = static_cast<std::uint32_t>(e);
  if (k < 100) {
    // Always print at least two digits in the exponent.
    // This is for compatibility with printf("%g").
    *buf++ = static_cast<char>('0' + k / 10);
    k %= 10;
    *buf++ = static_cast<char>('0' + k);
  } else {
    *buf++ = static_cast<char>('0' + k / 100);
    k %= 100;
    *buf++ = static_cast<char>('0' + k / 10);
    k %= 10;
    *buf++ = static_cast<char>('0' + k);
  }

  return buf;
}

/*!
@brief prettify v = buf * 10^decimal_exponent
If v is in the range [10^min_exp, 10^max_exp) it will be printed in fixed-point
notation. Otherwise it will be printed in exponential notation.
@pre min_exp < 0
@pre max_exp > 0
*/
inline char *format_buffer(char *buf, int len, int decimal_exponent,
                           int min_exp, int max_exp) {

  const int k = len;
  const int n = len + decimal_exponent;

  // v = buf * 10^(n-k)
  // k is the length of the buffer (number of decimal digits)
  // n is the position of the decimal point relative to the start of the buffer.

  if (k <= n && n <= max_exp) {
    // digits[000]
    // len <= max_exp + 2

    std::memset(buf + k, '0', static_cast<size_t>(n) - static_cast<size_t>(k));
    return buf + (static_cast<size_t>(n));
  }

  if (0 < n && n <= max_exp) {
    // dig.its
    // len <= max_digits10 + 1
    std::memmove(buf + (static_cast<size_t>(n) + 1), buf + n,
                 static_cast<size_t>(k) - static_cast<size_t>(n));
    buf[n] = '.';
    return buf + (static_cast<size_t>(k) + 1U);
  }

  if (min_exp < n && n <= 0) {
    // 0.[000]digits
    // len <= 2 + (-min_exp - 1) + max_digits10

    std::memmove(buf + (2 + static_cast<size_t>(-n)), buf,
                 static_cast<size_t>(k));
    buf[0] = '0';
    buf[1] = '.';
    std::memset(buf + 2, '0', static_cast<size_t>(-n));
    return buf + (2U + static_cast<size_t>(-n) + static_cast<size_t>(k));
  }

  if (k == 1) {
    // dE+123
    // len <= 1 + 5

    buf += 1;
  } else {
    // d.igitsE+123
    // len <= max_digits10 + 1 + 5

    std::memmove(buf + 2, buf + 1, static_cast<size_t>(k) - 1);
    buf[1] = '.';
    buf += 1 + static_cast<size_t>(k);
  }

  *buf++ = 'e';
  return append_exponent(buf, n - 1);
}

// Preserve the existing API and Godot's integer/zero/exponent spelling.
// Only initialized decimal digits are copied; no padded-output contract is needed.
template <typename FloatType>
char *to_chars(char *first, FloatType value) {
  if (std::signbit(value)) {
    value = -value;
    *first++ = '-';
  }
  if (value == 0) {
    *first++ = '0';
    return first;
  }
  const auto decimal = jkj::dragonbox::to_decimal(value, jkj::dragonbox::policy::sign::ignore);
  std::uint64_t significand = decimal.significand;
  char reversed[std::numeric_limits<FloatType>::max_digits10];
  int len = 0;
  do {
    reversed[len++] = static_cast<char>('0' + significand % 10);
    significand /= 10;
  } while (significand != 0);
  for (int i = 0; i < len; ++i) {
    first[i] = reversed[len - i - 1];
  }
  return format_buffer(first, len, decimal.exponent, -4, std::numeric_limits<double>::digits10);
}
} // namespace grisu2
