#include "Pipeline.h"
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
      out.push_back(t);
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(c))) {
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
            pair == "%=")
          t.text += take();
      }
    }
    out.push_back(t);
  }
  out.push_back({"<eof>", line, col, false, sourceName});
  return out;
}
} // namespace duck
