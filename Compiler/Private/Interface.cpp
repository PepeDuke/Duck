#include "Pipeline.h"
#include <stdexcept>

namespace duck {
namespace {
using Position = std::pair<int, int>; // line, column

Position at(const Token &t) { return {t.line, t.column}; }

bool inside(const Token &t, const Function &f) {
  return t.source == f.bodyOpen.source && at(f.bodyOpen) < at(t) && at(t) < at(f.bodyClose);
}

// Whether an importer reads this function's body. collectSymbols works out
// which heap pointer parameters never escape from the body, the parser and
// resolveReturnRoles work out whether a pointer result hands over ownership,
// and markup in a body can add fields to the containers it names.
bool bodyNeeded(const Function &f, const Program &unit) {
  if (f.bodyOpen.text != Tok::LeftBrace || f.bodyClose.text != Tok::RightBrace || f.bodyOpen.origin || f.bodyClose.origin) return true;
  if (f.result.depth) return true;

  for (const auto &p : f.params)
    if (p.type.depth && !p.reference) return true;

  for (const auto &host : unit.markupHosts)
    if (inside(host, f)) return true;

  for (const auto &value : unit.markupValues)
    if (inside(value.tag, f)) return true;

  return false;
}

void writeNumber(std::string &out, long long n) {
  out += std::to_string(n);
  out += ' ';
}

class Reader {
  const std::string &text;
  const std::string &path;
  size_t i = 0;

public:
  Reader(const std::string &text, const std::string &path) : text(text), path(path) {}

  [[noreturn]] void bad() const { throw std::runtime_error(path + ": not a valid Duck interface file"); }

  void expectWord(const std::string &word) {
    if (text.compare(i, word.size(), word) != 0) bad();
    i += word.size();
  }

  long long number() {
    bool negative = i < text.size() && text[i] == '-';
    if (negative) ++i;
    if (i >= text.size() || text[i] < '0' || text[i] > '9') bad();

    long long n = 0;

    while (i < text.size() && text[i] >= '0' && text[i] <= '9') n = n * 10 + (text[i++] - '0');

    if (i >= text.size() || (text[i] != ' ' && text[i] != '\n')) bad();
    ++i;
    return negative ? -n : n;
  }

  std::string bytes() {
    const auto length = number();

    if (length < 0 || i + size_t(length) + 1 > text.size() || text[i + size_t(length)] != '\n') bad();

    std::string result = text.substr(i, size_t(length));
    i += size_t(length) + 1;
    return result;
  }

  bool done() const { return i == text.size(); }
};
} // namespace

std::vector<Token> interfaceTokens(std::vector<Token> tokens, const Program &unit) {
  std::map<Position, Position> skipped; // A dropped body's '{' to its '}'.

  auto consider = [&](const Function &f) {
    if (!bodyNeeded(f, unit)) skipped.emplace(at(f.bodyOpen), at(f.bodyClose));
  };

  for (const auto &f : unit) consider(f);
  for (const auto &f : unit.tests) consider(f);

  // Kept tokens move down over dropped bodies, so the vector is reused.
  size_t kept = 0;

  for (size_t k = 0; k < tokens.size(); ++k) {
    const bool open = !tokens[k].string && tokens[k].text == Tok::LeftBrace;
    const auto opened = at(tokens[k]);

    if (kept != k) tokens[kept] = std::move(tokens[k]);

    ++kept;

    if (!open) continue;

    const auto found = skipped.find(opened);

    if (found == skipped.end()) continue;

    // Keep the body's line breaks so the braces stay valid statement syntax.
    size_t close = k + 1;

    while (close < tokens.size() && !(!tokens[close].string && tokens[close].text == Tok::RightBrace && at(tokens[close]) == found->second))
      ++close;

    if (close == tokens.size()) continue; // Not found: keep the body as written.

    k = close - 1;
  }

  tokens.erase(tokens.begin() + long(kept), tokens.end());
  return tokens;
}

std::string writeInterface(const std::string &key, const std::vector<std::vector<Token>> &files) {
  std::string out = "duck-interface 2\nkey ";
  out += std::to_string(key.size()) + ' ' + key + '\n';
  out += "files ";
  writeNumber(out, static_cast<long long>(files.size()));
  out.back() = '\n';

  for (const auto &tokens : files) {
    const SourceId source = tokens.empty() ? 0 : tokens.front().source;
    const auto &path = sourcePath(source);
    out += std::to_string(path.size()) + ' ' + path + '\n';
    writeNumber(out, static_cast<long long>(tokens.size()));
    out.back() = '\n';

    for (const auto &t : tokens) {
      if (t.source != source) throw std::logic_error("interface tokens mix source files");

      writeNumber(out, t.line);
      writeNumber(out, t.column);
      writeNumber(out, (t.string ? 1 : 0) | (t.character ? 2 : 0));
      // A fixed spelling is written as its Tok number alone; only a literal
      // (Tok::Literal, 0) is followed by its text.
      writeNumber(out, static_cast<long long>(t.text.kind()));

      if (t.text.kind() == Tok::Literal) {
        const std::string &text = t.text;
        out += std::to_string(text.size()) + ' ' + text + '\n';
      } else
        out.back() = '\n';
    }
  }

  return out;
}

std::vector<InterfaceFile> readInterface(const std::string &text, const std::string &path, std::string &key) {
  Reader in(text, path);
  in.expectWord("duck-interface 2\nkey ");
  key = in.bytes();
  in.expectWord("files ");
  const auto count = in.number();
  std::vector<InterfaceFile> files;

  TokenTextCache cache;

  for (long long f = 0; f < count; ++f) {
    InterfaceFile file;
    file.source = in.bytes();
    const auto tokens = in.number();
    file.tokens.reserve(size_t(tokens));

    for (long long k = 0; k < tokens; ++k) {
      Token t;
      t.line = int(in.number());
      t.column = int(in.number());
      const auto flags = in.number();
      t.string = flags & 1;
      t.character = flags & 2;
      const auto kind = in.number();

      if (kind < 0 || kind >= static_cast<long long>(Tok::Count)) in.bad();

      t.text = kind == static_cast<long long>(Tok::Literal) ? cache.intern(in.bytes()) : TokenText(static_cast<Tok>(kind));
      file.tokens.push_back(std::move(t));
    }

    files.push_back(std::move(file));
  }

  if (!in.done()) in.bad();

  return files;
}
} // namespace duck
