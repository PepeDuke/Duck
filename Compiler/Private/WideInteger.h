#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace duck {
// Nonnegative arbitrary-precision integer for literal bounds and constants
// beyond the host's 64-bit integers. Limbs are little-endian 32-bit words.
struct Magnitude {
  std::vector<uint32_t> limbs;

  static Magnitude parse(const std::string &digits, int base) {
    Magnitude m;

    for (char c : digits) {
      const uint32_t digit = c >= '0' && c <= '9' ? c - '0'
                           : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                                  : c - 'A' + 10;
      uint64_t carry = digit;

      for (auto &limb : m.limbs) {
        const uint64_t value = uint64_t(limb) * uint64_t(base) + carry;
        limb = uint32_t(value);
        carry = value >> 32;
      }

      if (carry) m.limbs.push_back(uint32_t(carry));
    }

    return m;
  }
  static Magnitude powerOfTwo(unsigned exponent) {
    Magnitude m;
    m.limbs.assign(exponent / 32 + 1, 0);
    m.limbs.back() = uint32_t{1} << (exponent % 32);
    return m;
  }
  // Number of significant bits; zero has none.
  unsigned bits() const {
    for (size_t i = limbs.size(); i > 0; --i)
      if (limbs[i - 1]) {
        unsigned n = 0;

        for (auto top = limbs[i - 1]; top; top >>= 1) ++n;

        return unsigned(32 * (i - 1)) + n;
      }

    return 0;
  }
  bool powerOfTwo() const {
    const auto n = bits();

    if (!n) return false;

    for (size_t i = 0; i < limbs.size(); ++i) {
      const uint32_t expected = i == (n - 1) / 32 ? uint32_t{1} << ((n - 1) % 32) : 0;

      if (limbs[i] != expected) return false;
    }

    return true;
  }
  std::string decimal() const {
    auto work = limbs;
    std::string digits;
    auto nonzero = [&] {
      return std::any_of(work.begin(), work.end(), [](uint32_t limb) { return limb != 0; });
    };

    do {
      uint64_t remainder = 0;

      for (size_t i = work.size(); i > 0; --i) {
        const uint64_t value = (remainder << 32) | work[i - 1];
        work[i - 1] = uint32_t(value / 10);
        remainder = value % 10;
      }

      digits += char('0' + remainder);
    } while (nonzero());

    return {digits.rbegin(), digits.rend()};
  }
};
} // namespace duck
