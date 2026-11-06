#include "Pipeline.h"
#include "Unicode.h"
#include <cctype>
namespace duck {
std::vector<Token> lex(const std::string &source,
                       const std::string &sourceName) {
  std::vector<Token> out;
  size_t i = 0;
  int line = 1, col = 1;
  auto take = [&]() {
    char c = source[i++];
    if (c == '\n') {
      ++line;
      col = 1;
    } else
      ++col;
    return c;
  };
  while (i < source.size()) {
    char c = source[i];
    Token t{"", line, col, false, sourceName};
    if (c == ' ' || c == '\t' || c == '\r') {
      take();
      continue;
    }
    if (c == '/' && i + 1 < source.size() && source[i + 1] == '/') {
      while (i < source.size() && source[i] != '\n')
        take();
      continue;
    }
    if (c == '/' && i + 1 < source.size() && source[i + 1] == '*') {
      take();
      take();
      while (i < source.size() && !(source[i] == '*' && i + 1 < source.size() &&
                                    source[i + 1] == '/')) {
        if (source[i] == '\n')
          out.push_back({"\n", line, col, false, sourceName});
        take();
      }
      if (i == source.size())
        fail(t, "unterminated comment");
      take();
      take();
      continue;
    }
    if (c == '"') {
      t.string = true;
      take();
      while (i < source.size() && source[i] != '"') {
        char ch = take();
        if (ch == '\n' || ch == '\r')
          fail(t, "newline in string literal");
        if (ch == '\\') {
          if (i == source.size())
            fail(t, "unterminated string literal");
          ch = take();
          switch (ch) {
          case 'u':
          case 'U': {
            const int digits = ch == 'u' ? 4 : 8;
            uint32_t cp = 0;
            for (int n = 0; n < digits; ++n) {
              if (i == source.size()) fail(t, "incomplete Unicode escape");
              char h = take();
              int digit = h >= '0' && h <= '9' ? h - '0'
                        : h >= 'a' && h <= 'f' ? h - 'a' + 10
                        : h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1;
              if (digit < 0) fail(t, "invalid Unicode escape");
              cp = (cp << 4) | static_cast<uint32_t>(digit);
            }
            if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
              fail(t, "Unicode escape must be a scalar value");
            appendUTF8(t.text, cp);
            continue;
          }
          case '0': ch = '\0'; break;
          case 'n':
            ch = '\n';
            break;
          case 'r':
            ch = '\r';
            break;
          case 't':
            ch = '\t';
            break;
          case '\\':
          case '"':
            break;
          default:
            fail(t, "unsupported escape sequence");
          }
        }
        t.text += ch;
      }
      if (i == source.size())
        fail(t, "unterminated string literal");
      take();
      decodeUTF8(t);
      out.push_back(t);
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(c))) {
      // Consume a prefixed integer as one token, including invalid digits,
      // so semantic diagnostics point to the complete literal. Hex e/E is
      // a digit, never a decimal exponent marker.
      if (i + 1 < source.size() &&
          integerLiteralBase(source.substr(i, 2)) != 10) {
        t.text += take();
        t.text += take();
        while (i < source.size() &&
               (std::isalnum(static_cast<unsigned char>(source[i])) ||
                source[i] == '_'))
          t.text += take();
        out.push_back(t);
        continue;
      }
      while (i < source.size() &&
             std::isdigit(static_cast<unsigned char>(source[i])))
        t.text += take();
      if (i + 1 < source.size() && source[i] == '.' &&
          std::isdigit(static_cast<unsigned char>(source[i + 1]))) {
        t.text += take();
        while (i < source.size() &&
               std::isdigit(static_cast<unsigned char>(source[i])))
          t.text += take();
      }
      if (i < source.size() && (source[i] == 'e' || source[i] == 'E')) {
        t.text += take();
        if (i < source.size() && (source[i] == '+' || source[i] == '-'))
          t.text += take();
        if (i == source.size() ||
            !std::isdigit(static_cast<unsigned char>(source[i])))
          fail(t, "exponent requires digits");
        while (i < source.size() &&
               std::isdigit(static_cast<unsigned char>(source[i])))
          t.text += take();
      }
      while (i < source.size() &&
             (std::isalnum(static_cast<unsigned char>(source[i])) ||
              source[i] == '_'))
        t.text += take();
    } else if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
      while (i < source.size() &&
             (std::isalnum(static_cast<unsigned char>(source[i])) ||
              source[i] == '_'))
        t.text += take();
    } else {
      t.text += take();
      if (i < source.size()) {
        std::string pair = t.text + source[i];
        if (pair == "==" || pair == "!=" || pair == "<=" || pair == ">=" ||
            pair == "++" || pair == "--" || pair == "&&" || pair == "||" ||
            pair == "+=" || pair == "-=" || pair == "*=" || pair == "/=" ||
            pair == "%=" || pair == "&=" || pair == "|=" || pair == "^=" ||
            pair == "<<" || pair == ">>")
          t.text += take();
        if ((t.text == "<<" || t.text == ">>") &&
            i < source.size() && source[i] == '=')
          t.text += take();
      }
    }
    out.push_back(t);
  }
  out.push_back({"<eof>", line, col, false, sourceName});
  return out;
}
} // namespace duck
