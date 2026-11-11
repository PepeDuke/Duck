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
  if (f.bodyOpen.text != "{" || f.bodyClose.text != "}" || f.bodyOpen.origin || f.bodyClose.origin) return true;
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

std::vector<Token> interfaceTokens(const std::vector<Token> &tokens, const Program &unit) {
  std::map<Position, Position> skipped; // A dropped body's '{' to its '}'.

  auto consider = [&](const Function &f) {
    if (!bodyNeeded(f, unit)) skipped.emplace(at(f.bodyOpen), at(f.bodyClose));
  };

  for (const auto &f : unit) consider(f);
  for (const auto &f : unit.tests) consider(f);

  std::vector<Token> result;
  result.reserve(tokens.size());

  for (size_t k = 0; k < tokens.size(); ++k) {
    result.push_back(tokens[k]);

    if (tokens[k].string || tokens[k].text != "{") continue;

    const auto found = skipped.find(at(tokens[k]));

    if (found == skipped.end()) continue;

    // Keep the body's line breaks so the braces stay valid statement syntax.
    size_t close = k + 1;

    while (close < tokens.size() && !(!tokens[close].string && tokens[close].text == "}" && at(tokens[close]) == found->second))
      ++close;

    if (close == tokens.size()) continue; // Not found: keep the body as written.

    k = close - 1;
  }

  return result;
}

std::string writeInterface(const std::string &key, const std::vector<std::vector<Token>> &files) {
  std::string out = "duck-interface 1\nkey ";
  out += std::to_string(key.size()) + ' ' + key + '\n';
  out += "files ";
  writeNumber(out, static_cast<long long>(files.size()));
  out.back() = '\n';

  for (const auto &tokens : files) {
    const std::string source = tokens.empty() ? std::string() : tokens.front().source;
    out += std::to_string(source.size()) + ' ' + source + '\n';
    writeNumber(out, static_cast<long long>(tokens.size()));
    out.back() = '\n';

    for (const auto &t : tokens) {
      if (t.source != source) throw std::logic_error("interface tokens mix source files");

      writeNumber(out, t.line);
      writeNumber(out, t.column);
      writeNumber(out, (t.string ? 1 : 0) | (t.character ? 2 : 0));
      out += std::to_string(t.text.size()) + ' ' + t.text + '\n';
    }
  }

  return out;
}

std::vector<std::vector<Token>> readInterface(const std::string &text, const std::string &path, std::string &key) {
  Reader in(text, path);
  in.expectWord("duck-interface 1\nkey ");
  key = in.bytes();
  in.expectWord("files ");
  const auto count = in.number();
  std::vector<std::vector<Token>> files;

  for (long long f = 0; f < count; ++f) {
    const auto source = in.bytes();
    const auto tokens = in.number();
    std::vector<Token> file;
    file.reserve(size_t(tokens));

    for (long long k = 0; k < tokens; ++k) {
      Token t;
      t.line = int(in.number());
      t.column = int(in.number());
      const auto flags = in.number();
      t.string = flags & 1;
      t.character = flags & 2;
      t.text = in.bytes();
      t.source = source;
      file.push_back(std::move(t));
    }

    files.push_back(std::move(file));
  }

  if (!in.done()) in.bad();

  return files;
}
} // namespace duck
