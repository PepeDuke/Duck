#pragma once
#include "Syntax.h"
#include <cstdint>
namespace duck {
inline std::vector<uint32_t> decodeUTF8(const Token &token) {
  std::vector<uint32_t> result;
  const auto &bytes = token.text;
  for (size_t i = 0; i < bytes.size();) {
    auto first = static_cast<unsigned char>(bytes[i++]);
    uint32_t cp = first;
    int extra = 0;
    uint32_t minimum = 0;
    if (first >= 0xC2 && first <= 0xDF) { cp &= 31; extra = 1; minimum = 0x80; }
    else if (first >= 0xE0 && first <= 0xEF) { cp &= 15; extra = 2; minimum = 0x800; }
    else if (first >= 0xF0 && first <= 0xF4) { cp &= 7; extra = 3; minimum = 0x10000; }
    else if (first >= 0x80) fail(token, "invalid UTF-8 in text literal");
    for (int n = 0; n < extra; ++n) {
      if (i == bytes.size()) fail(token, "incomplete UTF-8 in text literal");
      auto next = static_cast<unsigned char>(bytes[i++]);
      if ((next & 0xC0) != 0x80) fail(token, "invalid UTF-8 continuation in text literal");
      cp = (cp << 6) | (next & 63);
    }
    if (cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
      fail(token, "invalid Unicode scalar in text literal");
    result.push_back(cp);
  }
  return result;
}
inline void appendUTF8(std::string &out, uint32_t cp) {
  if (cp < 0x80) out += static_cast<char>(cp);
  else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 63));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 63));
    out += static_cast<char>(0x80 | (cp & 63));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 63));
    out += static_cast<char>(0x80 | ((cp >> 6) & 63));
    out += static_cast<char>(0x80 | (cp & 63));
  }
}
} // namespace duck
