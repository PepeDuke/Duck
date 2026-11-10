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
// Fixed-width bit patterns avoid host signed overflow and undefined shifts.
// Every operation is trimmed to the Duck type's width (8 through 512 bits).
struct Bits {
  std::vector<uint32_t> words = std::vector<uint32_t>(16, 0);
  Bits(uint64_t n = 0) { words[0] = uint32_t(n); words[1] = uint32_t(n >> 32); }
  bool bit(unsigned i) const { return (words[i / 32] >> (i % 32)) & 1; }
  void set(unsigned i) { words[i / 32] |= uint32_t(1) << (i % 32); }
  bool zero() const { for (auto w : words) if (w) return false; return true; }
  void trim(unsigned width) {
    for (unsigned i = width; i < 512; ++i) words[i / 32] &= ~(uint32_t(1) << (i % 32));
  }
  int compare(const Bits &b) const {
    for (size_t i = words.size(); i > 0; --i)
      if (words[i - 1] != b.words[i - 1]) return words[i - 1] < b.words[i - 1] ? -1 : 1;

    return 0;
  }
  Bits add(const Bits &b) const {
    Bits r; uint64_t carry = 0;

    for (size_t i = 0; i < words.size(); ++i) {
      carry += uint64_t(words[i]) + b.words[i];
      r.words[i] = uint32_t(carry); carry >>= 32;
    }

    return r;
  }
  Bits negate() const {
    Bits r;

    for (size_t i = 0; i < words.size(); ++i) r.words[i] = ~words[i];

    return r.add(Bits(1));
  }
  Bits multiply(const Bits &b) const {
    Bits r;

    for (size_t i = 0; i < words.size(); ++i) {
      uint64_t carry = 0;

      for (size_t j = 0; i + j < words.size(); ++j) {
        const uint64_t n = uint64_t(words[i]) * b.words[j] + r.words[i + j] + carry;
        r.words[i + j] = uint32_t(n); carry = n >> 32;
      }
    }

    return r;
  }
  Bits shift(unsigned count, unsigned width, bool right, bool sign = false) const {
    Bits r;

    for (unsigned i = 0; i < width; ++i) {
      const bool value = right ? (i + count < width ? bit(i + count) : sign)
                               : (i >= count && bit(i - count));

      if (value) r.set(i);
    }

    return r;
  }
  std::pair<Bits, Bits> divide(const Bits &b, unsigned width) const {
    Bits q, r;

    for (unsigned i = width; i > 0; --i) {
      // Preserve the carry out of a full-width remainder.
      const bool carry = r.bit(width - 1);
      r = r.shift(1, width, false);

      if (bit(i - 1)) r.set(0);
      if (carry || r.compare(b) >= 0) { r = r.add(b.negate()); r.trim(width); q.set(i - 1); }
    }

    return {q, r};
  }
  static Bits parse(const std::string &s) {
    const bool negative = s[0] == '-';
    const auto m = Magnitude::parse(negative ? s.substr(1) : s, 10);
    Bits r;

    for (size_t i = 0; i < m.limbs.size() && i < r.words.size(); ++i) r.words[i] = m.limbs[i];

    return negative ? r.negate() : r;
  }
  std::string decimal() const {
    Magnitude m; m.limbs.assign(words.begin(), words.end()); return m.decimal();
  }
};
} // namespace duck
