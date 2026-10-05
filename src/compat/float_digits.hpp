// Internal: the shortest decimal digits that read back as a double.
#pragma once

#include <string>

namespace campfire::compat::detail {

struct Digits {
  std::string digits;  // "12345" for 1.2345e6, no leading or trailing zeros
  int decpt;           // where the decimal point goes: 1.2345e6 has decpt 7
};

// The shortest digits of a finite, positive double (Ryu, through std::to_chars).
Digits shortest_digits(double magnitude);

// Like shortest_digits, but of two equally close shortest forms it takes the even one, as Ruby's
// dtoa does (667020902720176.25.to_s is "667020902720176.2").
Digits ruby_shortest_digits(double magnitude);

}  // namespace campfire::compat::detail
