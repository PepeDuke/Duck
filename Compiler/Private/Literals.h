#pragma once
#include "Syntax.h"
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>

// Reading and writing literal text, shared by every compiler stage that sees
// a number or an identifier.
namespace duck {

inline bool isIdentifierStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }

inline bool isIdentifierChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

inline bool isWord(const Token &t) { return !t.string && !t.text.empty() && isIdentifierStart(t.text[0]); }

inline bool isEquality(const std::string &op) { return op == "==" || op == "!="; }

inline bool isOrdering(const std::string &op) { return op == "<" || op == ">" || op == "<=" || op == ">="; }

inline bool isComparison(const std::string &op) { return isEquality(op) || isOrdering(op); }

// The value of a hexadecimal digit, or -1 for any other character.
inline int digitValue(char c) {
  return c >= '0' && c <= '9'   ? c - '0'
         : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                : -1;
}

// Parses in the classic locale. False for malformed text or a value out of
// range for the type; `single` parses at float precision.
inline bool parseFloatingLiteral(const std::string &text, bool single, double &value) {
  std::istringstream input(text);
  input.imbue(std::locale::classic());

  if (single) {
    float narrow = 0;
    input >> narrow;
    value = narrow;
  } else
    input >> value;

  return input && input.peek() == std::char_traits<char>::eof() && std::isfinite(value);
}

// LLVM's hexadecimal spelling of a double, which round-trips exactly.
inline std::string floatingConstantText(double value) {
  uint64_t bits = 0;
  static_assert(sizeof(value) == sizeof(bits));
  std::memcpy(&bits, &value, sizeof(bits));

  std::ostringstream encoded;
  encoded << "0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(16) << bits;
  return encoded.str();
}

} // namespace duck
