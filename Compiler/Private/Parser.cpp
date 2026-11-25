#include "Literals.h"
#include "Pipeline.h"
#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <utility>
namespace duck {
namespace {
class Parser {
  std::vector<Token> tokens;
  size_t pos = 0;
  // Each macro expansion, in order: where its output starts, how many tokens
  // it is, and the call it replaced, so the tokens can be given back as written.
  struct Splice {
    size_t first, count;
    std::vector<Token> replaced;
  };
  std::vector<Splice> splices;
  const std::set<std::string> &containerNames;
  const MacroTable &macros;
  // Words that stop being keywords when the program declares a container of
  // that name. The names are fixed for the whole parse, so each is looked up once.
  const bool taskContainer, fileContainer, markupContainer, pContainer, tContainer;
  // typePath's working storage, kept between calls so a lookup allocates
  // nothing: the token index of each word of the path, and a candidate name.
  mutable std::vector<size_t> pathWords;
  mutable std::string probe;
  // typePath's last answer. The parser asks about one position several times
  // (declarationType, namedType, ...) before moving on. The answer depends on
  // the tokens there, which change only by a macro splice, the enclosing
  // scope, and the visible usings; holding the usings keeps their address from
  // being reused by a later list.
  struct TypePathCache {
    size_t at = SIZE_MAX, splices = 0, length = 0;
    std::string scope, qualified;
    Usings usings;
  };
  mutable TypePathCache typePathCache;
  // Names declared by macro output, innermost lexical scope last, each with
  // the mark of the expansion that declared it.
  std::vector<std::set<std::pair<unsigned, std::string>>> hygiene{1};
  unsigned expansionMarks = 0;
  size_t expandedTokens = 0;
  // The qualified container whose declarations or member bodies are being
  // parsed, such as "Name.Space", or empty at file scope.
  std::string scope;
  // The using directives visible here, outermost scope first.
  Usings usings = std::make_shared<const std::vector<Using>>();
  bool returnsOwner = false;
  // The stable name of the test whose body is being parsed, or empty.
  std::string currentTest;
  Program *output = nullptr; // The unit being parsed, for what markup records on it.
  unsigned markupLocals = 0; // Numbers the hidden locals of generated markup code.
  int lambdaDepth = 0;
  bool returnsMarkup = false; // The function being parsed returns markup.
  bool expectMarkup = false;  // The next expression has a declared markup type.
  const Token &peek() const { return tokens[pos]; }
  bool at(Tok t) const { return !peek().string && peek().text == t; }
  Token pop() { return tokens[pos++]; }
  bool accept(Tok t) {
    if (!at(t)) return false;

    pop();
    return true;
  }
  static std::string describe(const Token &t) {
    if (t.string) return t.character ? "a character literal" : "a string literal";
    if (t.text == Tok::Eof) return "end of file";
    if (t.text == Tok::Newline) return "end of line";

    return "'" + t.text + "'";
  }
  // For a missing closer, where the opener it would match is.
  std::string stillOpen(const std::string &closer) const {
    const char opener = closer == ")" ? '(' : closer == "]" ? '[' : closer == "}" ? '{' : '\0';

    if (!opener) return "";

    int depth = 0;

    for (size_t k = pos; k-- > 0;) {
      const auto &t = tokens[k];

      if (t.string || t.text.empty()) continue;

      if (t.text == closer) ++depth;
      else if (t.text.back() == opener && depth-- == 0)
        return " (the '" + t.text + "' at " + where(t) + " is still open)";
    }

    return "";
  }
  [[noreturn]] void expected(const std::string &s) const {
    fail(peek(), "expected '" + s + "', found " + describe(peek()) + stillOpen(s));
  }
  Token expect(Tok t) {
    if (!at(t)) expected(TokenText(t).str());

    return pop();
  }
  [[noreturn]] void badStatementEnd(const std::string &suffix = "") const {
    if (at(Tok::Semicolon)) fail(peek(), "expected newline after statement, found ';'; Duck statements end at a newline, so remove it");

    fail(peek(), "expected newline or '}' after statement, found " + describe(peek()) + suffix);
  }
  std::vector<std::string> knownTypes() const {
    std::vector<std::string> names{"int", "long", "short", "byte", "uint", "int8", "int16", "int32", "int64",
                                   "uint8", "uint16", "uint32", "uint64", "float", "double", "bool", "void",
                                   "string", "name", "cstring", "char", "date", "time", "datetime", "big",
                                   "ubig", "tiny", "utiny", "func", "vec2", "vec3", "vec4", "mat3", "mat4",
                                   "quat", "rot", "file"};
    names.insert(names.end(), containerNames.begin(), containerNames.end());
    return names;
  }
  [[noreturn]] void unknownType(const Token &t) const {
    fail(t, "unknown type '" + t.text + "'" + didYouMean(t.text, knownTypes()));
  }
  // Where a type is required: reports a misspelled one, when it is clear
  // what was meant, instead of the position's general message.
  void rejectMisspelledType(const Token &t) const {
    if (isWord(t) && !closestName(t.text, knownTypes()).empty()) unknownType(t);
  }
  void lines() {
    while (accept(Tok::Newline)) {
    }
  }
  Token identifier() {
    auto t = peek();

    if (!isWord(t))
      fail(t, "expected identifier, found " + describe(t));

    if (isType() || t.text == Tok::True || t.text == Tok::False ||
        t.text == Tok::Return || t.text == Tok::If || t.text == Tok::Else ||
        t.text == Tok::While || t.text == Tok::For || t.text == Tok::Break ||
        t.text == Tok::Continue || t.text == Tok::Do || t.text == Tok::Extern ||
        t.text == Tok::Null || t.text == Tok::Using || t.text == Tok::Switch)
      fail(t, "reserved keyword cannot be an identifier");

    return pop();
  }
  // A local declared by macro output gets a name source text cannot spell, so
  // it neither captures the caller's names nor is visible to them.
  std::string declareLocal(const Token &t) {
    if (!t.mark || t.text.find('$') != std::string::npos) return t.text;

    hygiene.back().insert({t.mark, t.text});
    return t.text + "$" + std::to_string(t.mark);
  }
  // A name in macro output means that expansion's own local when one is in
  // scope; any other name is looked up where the macro was expanded.
  void resolveLocal(Token &t) const {
    if (!t.mark) return;

    for (auto level = hygiene.rbegin(); level != hygiene.rend(); ++level)
      if (level->count({t.mark, t.text})) {
        t.text += "$" + std::to_string(t.mark);
        return;
      }
  }
  static bool word(const Token &t) {
    return isWord(t);
  }
  static bool punctuation(const Token &t, Tok kind) { return !t.string && t.text == kind; }
  static bool openingBracket(const Token &t) { return punctuation(t, Tok::LeftParen) || punctuation(t, Tok::LeftBracket) || punctuation(t, Tok::LeftBrace); }
  static bool closingBracket(const Token &t) { return punctuation(t, Tok::RightParen) || punctuation(t, Tok::RightBracket) || punctuation(t, Tok::RightBrace); }
  struct MacroUse {
    const Macro *macro;
    std::string qualified;
    size_t open; // Index of the call's '('.
  };
  // The macro called by `Name(` or `Path.To.Name(` at `at`. A call in macro
  // output looks in the macro's own container first, so a macro keeps
  // working when it is expanded in another container.
  std::optional<MacroUse> macroUse(const std::vector<Token> &list, size_t at) const {
    if (macros.empty() || at >= list.size() || !word(list[at])) return std::nullopt;

    std::string path = list[at].text;
    size_t k = at + 1;

    while (k + 1 < list.size() && punctuation(list[k], Tok::Dot) && word(list[k + 1])) {
      path += "." + list[k + 1].text;
      k += 2;
    }

    if (k >= list.size() || !punctuation(list[k], Tok::LeftParen)) return std::nullopt;

    auto parent = [](const std::string &name) {
      const auto dot = name.rfind('.');
      return dot == std::string::npos ? std::string() : name.substr(0, dot);
    };
    std::vector<std::string> starts;

    if (list[at].origin) starts.push_back(parent(list[at].origin->macro));

    starts.push_back(scope);

    for (auto enclosing : starts)
      for (;;) {
        const auto name = enclosing.empty() ? path : enclosing + "." + path;

        if (auto found = macros.find(name); found != macros.end())
          return MacroUse{&found->second, found->first, k};

        const Macro *imported = nullptr;
        std::string importedName;

        for (const auto &directive : *usings) {
          if (directive.level != enclosing) continue;

          const auto candidate = directive.target + "." + path;
          auto found = macros.find(candidate);

          if (found == macros.end()) continue;

          if (imported && importedName != candidate)
            fail(list[at], "macro '" + path + "' is ambiguous between '" + importedName + "' and '" + candidate + "'");

          imported = &found->second;
          importedName = candidate;
        }

        if (imported) return MacroUse{imported, importedName, k};
        if (enclosing.empty()) break;

        enclosing = parent(enclosing);
      }

    return std::nullopt;
  }
  // The arguments of the call: each one's tokens, split at top-level commas.
  std::vector<std::vector<Token>> macroArguments(const MacroUse &use, size_t &close) const {
    std::vector<std::vector<Token>> arguments(1);
    int depth = 1;
    bool lineBreak = false;

    for (size_t k = use.open + 1;; ++k) {
      if (k >= tokens.size() || punctuation(tokens[k], Tok::Eof)) fail(tokens[use.open], "expected ')'");

      const auto &t = tokens[k];

      if (depth == 1 && punctuation(t, Tok::Newline)) {
        lineBreak = !arguments.back().empty();
        continue;
      }

      if (openingBracket(t)) ++depth;
      else if (closingBracket(t)) {
        if (--depth == 0) {
          close = k;
          break;
        }
      } else if (depth == 1 && punctuation(t, Tok::Comma)) {
        if (arguments.back().empty()) fail(t, "empty macro argument");

        arguments.emplace_back();
        lineBreak = false;
        continue;
      }

      if (lineBreak) fail(t, "a macro argument must stay on one line unless it is inside parentheses");

      arguments.back().push_back(t);
    }

    if (arguments.size() == 1 && arguments[0].empty()) arguments.clear();
    else if (arguments.back().empty()) fail(tokens[close], "empty macro argument");

    if (arguments.size() != use.macro->parameters.size())
      fail(tokens[use.open - 1], "macro '" + use.qualified + "' takes " +
                                     std::to_string(use.macro->parameters.size()) + " argument(s), got " +
                                     std::to_string(arguments.size()) + "; it is defined at " + where(use.macro->name));

    return arguments;
  }
  std::shared_ptr<const Expansion> beginExpansion(const Token &call, const MacroUse &use) {
    unsigned depth = 1;

    for (const auto *e = call.origin.get(); e; e = e->call.origin.get(), ++depth)
      if (e->macro == use.qualified) fail(call, "macro '" + use.qualified + "' expands itself; macros cannot be recursive");

    if (depth > 32) fail(call, "macro expansion exceeded the 32-level depth limit");

    expandedTokens += use.macro->body.size() + 1;

    if (expandedTokens > 200000) fail(call, "macro expansion exceeded the 200000-token limit");

    return std::make_shared<const Expansion>(Expansion{call, use.qualified, use.macro->name, ++expansionMarks});
  }
  static Token synthetic(const std::shared_ptr<const Expansion> &origin, const std::string &text) {
    Token t = origin->definition;
    t.text = text;
    t.string = t.character = false;
    t.origin = origin;
    return t;
  }
  static Token fromMacro(Token t, const std::shared_ptr<const Expansion> &origin) {
    t.mark = origin->mark;
    t.origin = origin;
    return t;
  }
  // One read-only local per parameter, so each argument is type-checked and
  // evaluated exactly once, in order, however often the body names it.
  static void appendBindings(std::vector<Token> &out, const Macro &macro, std::vector<std::vector<Token>> arguments,
                             const std::shared_ptr<const Expansion> &origin) {
    for (size_t i = 0; i < arguments.size(); ++i) {
      const auto &parameter = macro.parameters[i];
      out.push_back(synthetic(origin, "#"));

      for (const auto &t : parameter.type) out.push_back(fromMacro(t, origin));

      out.push_back(fromMacro(parameter.name, origin));
      out.push_back(synthetic(origin, "="));

      for (auto &t : arguments[i]) out.push_back(std::move(t));

      out.push_back(synthetic(origin, "\n"));
    }
  }
  // Puts `with` in place of the `count` tokens at `first` and returns those.
  // The shared length is overwritten, so the rest of the file moves only once,
  // by the difference.
  std::vector<Token> splice(size_t first, size_t count, std::vector<Token> with) {
    const auto begin = tokens.begin() + long(first);
    std::vector<Token> removed(std::make_move_iterator(begin), std::make_move_iterator(begin + long(count)));
    const size_t shared = std::min(count, with.size());
    std::move(with.begin(), with.begin() + long(shared), begin);

    if (with.size() > count)
      tokens.insert(begin + long(count), std::make_move_iterator(with.begin() + long(shared)),
                    std::make_move_iterator(with.end()));
    else
      tokens.erase(begin + long(with.size()), begin + long(count));

    return removed;
  }
  void replaceTokens(size_t first, size_t last, std::vector<Token> replacement) {
    const size_t count = replacement.size();
    splices.push_back({first, count, splice(first, last + 1 - first, std::move(replacement))});
  }
  // In statement position a macro becomes a block: its parameters and locals
  // end with it, while break, continue, and return act on the caller.
  void expandStatementMacro() {
    auto use = macroUse(tokens, pos);

    if (!use) return;

    size_t close = 0;
    auto arguments = macroArguments(*use, close);

    if (close + 1 >= tokens.size() ||
        !(punctuation(tokens[close + 1], Tok::Newline) || punctuation(tokens[close + 1], Tok::RightBrace) ||
          punctuation(tokens[close + 1], Tok::Eof)))
      return; // Part of a larger expression.

    const auto origin = beginExpansion(tokens[pos], *use);
    std::vector<Token> out{synthetic(origin, "{"), synthetic(origin, "\n")};
    appendBindings(out, *use->macro, std::move(arguments), origin);

    for (const auto &t : use->macro->body) out.push_back(fromMacro(t, origin));

    out.push_back(synthetic(origin, "\n"));
    out.push_back(synthetic(origin, "}"));
    replaceTokens(pos, close, std::move(out));
  }
  // In a Switch body a macro supplies sections, so there is no block to hold
  // its parameters: they bind on the Switch, after the selector is evaluated.
  void expandCaseMacro(Stmt &switchStatement, const MacroUse &use) {
    size_t close = 0;
    auto arguments = macroArguments(use, close);
    const auto origin = beginExpansion(tokens[pos], use);
    const auto count = arguments.size();
    std::vector<Token> out;
    appendBindings(out, *use.macro, std::move(arguments), origin);

    for (const auto &t : use.macro->body) out.push_back(fromMacro(t, origin));

    out.push_back(synthetic(origin, "\n"));
    replaceTokens(pos, close, std::move(out));

    for (size_t i = 0; i < count; ++i) {
      switchStatement.alternate.push_back(simpleStatement());
      expect(Tok::Newline);
    }
  }
  // Whether a macro's output starts with a Switch label, directly or through
  // the macro it calls first.
  bool opensSection(const MacroUse &use, unsigned depth = 0) const {
    const auto &body = use.macro->body;
    size_t k = 0;

    while (k < body.size() && punctuation(body[k], Tok::Newline)) ++k;
    if (k == body.size()) return false;
    if (punctuation(body[k], Tok::Case)) return true;
    if (punctuation(body[k], Tok::Default) && k + 1 < body.size() && punctuation(body[k + 1], Tok::Colon)) return true;
    if (depth >= 32) return false;

    const auto inner = macroUse(body, k);
    return inner && opensSection(*inner, depth + 1);
  }
  // A macro used as a value must be one expression. Its parameters bind in
  // a scope of their own inside that expression, so an argument runs once,
  // only when the expression is reached, however often the body names it.
  void expandValueMacro(const MacroUse &use) {
    size_t close = 0;
    auto arguments = macroArguments(use, close);
    const auto origin = beginExpansion(tokens[pos], use);
    const auto &macro = *use.macro;
    size_t first = 0, last = macro.body.size();

    while (first < last && punctuation(macro.body[first], Tok::Newline)) ++first;
    while (last > first && punctuation(macro.body[last - 1], Tok::Newline)) --last;
    if (first == last) fail(tokens[pos], "macro '" + use.qualified + "' is empty, so it cannot be used as a value");

    int depth = 0;

    for (size_t k = first; k < last; ++k) {
      const auto &t = macro.body[k];

      if (openingBracket(t)) ++depth;
      else if (closingBracket(t)) --depth;
      else if (depth == 0 && punctuation(t, Tok::Newline))
        fail(tokens[pos], "macro '" + use.qualified + "' expands to statements, so it cannot be used as a value; "
                          "it is defined at " + where(macro.name));
    }

    // With nothing to bind, the output is simply the parenthesized body.
    const bool binds = !arguments.empty();
    std::vector<Token> out{synthetic(origin, binds ? "$value" : "(")};
    appendBindings(out, macro, std::move(arguments), origin);

    if (binds) out.push_back(synthetic(origin, "$result"));
    for (size_t k = first; k < last; ++k) out.push_back(fromMacro(macro.body[k], origin));

    out.push_back(synthetic(origin, binds ? "$end" : ")"));
    replaceTokens(pos, close, std::move(out));
  }
  bool atSectionLabel() const {
    return at(Tok::Case) || (at(Tok::Default) && punctuation(tokens.at(pos + 1), Tok::Colon));
  }
  // `case a, b:` and `default:`, including labels stacked on one section.
  void sectionLabels(Stmt &switchStatement, Stmt &section) {
    for (;;) {
      if (accept(Tok::Default)) {
        if (section.isDefault) fail(section.token, "a Switch section cannot repeat default");

        section.isDefault = true;
      } else {
        expect(Tok::Case);

        do {
          lines();
          section.labels.push_back(expression());
          lines();
        } while (accept(Tok::Comma));
      }

      expect(Tok::Colon);
      lines();

      // A macro may supply the next stacked label.
      while (auto use = macroUse(tokens, pos)) {
        if (!opensSection(*use)) break;

        expandCaseMacro(switchStatement, *use);
        lines();
      }

      if (!atSectionLabel()) return;
    }
  }
  std::unique_ptr<Stmt> switchStatement() {
    auto s = std::make_unique<Stmt>();
    s->kind = StmtKind::Switch;
    s->token = pop();
    expect(Tok::LeftParen);
    lines();
    auto selector = std::make_unique<Stmt>();
    selector->kind = StmtKind::Let;
    selector->token = peek();
    selector->name = "$switch";
    selector->expr = expression();
    lines();
    expect(Tok::RightParen);
    s->target = std::make_unique<Expr>();
    s->target->kind = ExprKind::Name;
    s->target->token = selector->token;
    s->target->token.text = selector->name;
    s->target->token.string = s->target->token.character = false;
    s->initializer = std::move(selector);
    lines();
    expect(Tok::LeftBrace);
    lines();
    hygiene.emplace_back();

    while (!at(Tok::RightBrace)) {
      if (at(Tok::Eof)) expected("}");

      if (auto use = macroUse(tokens, pos)) {
        expandCaseMacro(*s, *use);
        lines();
        continue;
      }

      if (!atSectionLabel()) fail(peek(), "expected 'case' or 'default' in Switch body");

      auto section = std::make_unique<Stmt>();
      section->kind = StmtKind::Case;
      section->token = peek();
      sectionLabels(*s, *section);
      hygiene.emplace_back();

      while (!at(Tok::RightBrace) && !atSectionLabel()) {
        if (at(Tok::Eof)) expected("}");
        if (auto use = macroUse(tokens, pos); use && opensSection(*use)) break;

        section->body.push_back(statement());

        if (!at(Tok::RightBrace) && !at(Tok::Newline)) fail(peek(), "expected newline between statements");

        lines();
      }

      hygiene.pop_back();
      s->body.push_back(std::move(section));
    }

    hygiene.pop_back();
    expect(Tok::RightBrace);

    if (s->body.empty()) fail(s->token, "a Switch needs at least one case or default");

    return s;
  }
  // The tokens a container or enum type path at `at` spans, or 0. A path
  // such as `Space` or `Name.Space` resolves like a relative function name:
  // from the innermost enclosing container outward, then at file scope,
  // longest path first. `qualified` receives the type's full name.
  size_t typePath(size_t at, std::string *qualified = nullptr) const {
    auto &cache = typePathCache;

    if (cache.at != at || cache.splices != splices.size() || cache.usings != usings || cache.scope != scope) {
      cache.at = SIZE_MAX; // Stays invalid if the lookup fails.
      const auto length = findTypePath(at, cache.qualified);
      cache.at = at;
      cache.splices = splices.size();
      cache.usings = usings;
      cache.scope = scope;
      cache.length = length;
    }

    if (qualified && cache.length) *qualified = cache.qualified;

    return cache.length;
  }
  // Sets `probe` to the first `count` words of the path, after the first
  // `prefixLength` characters of `prefix` and a dot. `dot` keeps the dot even
  // after an empty prefix, as a using's target always has one.
  void buildProbe(const std::string &prefix, size_t prefixLength, size_t count, bool dot) const {
    probe.assign(prefix, 0, prefixLength);

    for (size_t n = 0; n < count; ++n) {
      if (n > 0 || prefixLength > 0 || dot) probe += '.';

      probe += tokens[pathWords[n]].text.str();
    }
  }
  // typePath's lookup. Candidate names are built in `probe` and the enclosing
  // scope is a length of `scope`, so trying a name allocates nothing.
  size_t findTypePath(size_t at, std::string &found) const {
    pathWords.clear();

    for (size_t k = at;; k += 2) {
      if (!isWord(tokens[k])) break;

      pathWords.push_back(k);

      if (tokens[k + 1].string || tokens[k + 1].text != Tok::Dot) break;
    }

    if (pathWords.empty()) return 0;

    for (size_t enclosing = scope.size();;) {
      for (size_t n = pathWords.size(); n-- > 0;) {
        buildProbe(scope, enclosing, n + 1, false);

        if (containerNames.count(probe)) {
          found = probe;
          return 2 * n + 1;
        }
      }

      // Then the containers this scope imports. Two imports providing the
      // path make it ambiguous.
      const Using *via = nullptr;
      size_t length = 0;

      for (const auto &directive : *usings) {
        if (directive.level.size() != enclosing || scope.compare(0, enclosing, directive.level) != 0) continue;

        for (size_t n = pathWords.size(); n-- > 0;) {
          buildProbe(directive.target, directive.target.size(), n + 1, true);

          if (!containerNames.count(probe)) continue;

          if (via && found != probe) {
            std::string path;

            for (size_t w = 0; w <= n; ++w) path += (w ? "." : "") + tokens[pathWords[w]].text;

            fail(tokens[at], "'" + path + "' is ambiguous between '" + found + "' (using " + via->target +
                                 ") and '" + probe + "' (using " + directive.target + ")");
          }

          found = probe;
          via = &directive;
          length = 2 * n + 1;
          break;
        }
      }

      if (via) return length;

      if (enclosing == 0) break;

      const auto dot = scope.rfind('.', enclosing - 1);
      enclosing = dot == std::string::npos ? 0 : dot;
    }

    return 0;
  }
  bool namedType() const { return typePath(pos) != 0; }
  // `Space s` where Space is only a nested type outside this scope: name the
  // qualified path instead of reporting a generic syntax error.
  void unreachableNestedType() const {
    const auto &t = peek(), &next = tokens.at(pos + 1);
    auto word = [](const Token &w) {
      return !w.string && !w.text.empty() &&
             isIdentifierStart(w.text[0]);
    };

    if (!word(t) || !word(next)) return;

    for (const auto &name : containerNames)
      if (name[0] != '#' && name.size() > t.text.size() &&
          name.compare(name.size() - t.text.size() - 1, std::string::npos, "." + t.text) == 0)
        fail(t, "unknown type '" + t.text + "'; did you mean '" + name + "'?");
  }
  // int.Max, i3.Min, double.Max: a built-in type followed by a member.
  bool typeMember() const {
    const auto &next = tokens.at(pos + 1);
    return isType() && !next.string && next.text == Tok::Dot;
  }
  bool declarationType() const {
    const auto length = typePath(pos);

    if (!length) return isType() && !typeMember();

    const auto &next = tokens.at(pos + length);
    return !next.string && (next.text == Tok::Star || next.text == Tok::Ampersand || arraySuffix(pos + length) ||
           isIdentifierStart(next.text[0]));
  }
  // `[]`, `[N]`, `[-]`, or a size hint `[*]` or `[N*]` after an element type,
  // followed by a name or `&`.
  bool arraySuffix(size_t at) const {
    auto punct = [&](size_t k, auto text) {
      return k < tokens.size() && !tokens[k].string && tokens[k].text == text;
    };

    if (!punct(at, Tok::LeftBracket)) return false;

    size_t close = at + 1;

    if (punct(close, Tok::Minus)) ++close;
    else {
      if (close < tokens.size() && !tokens[close].string && std::isdigit(static_cast<unsigned char>(tokens[close].text[0])))
        ++close;

      if (punct(close, Tok::Star)) ++close;
    }

    if (!punct(close, Tok::RightBracket) || close + 1 >= tokens.size()) return false;

    const auto &next = tokens[close + 1];
    // A following `[` is a further dimension, reported once the type is read.
    return !next.string && (next.text == Tok::Ampersand || next.text == Tok::LeftBracket ||
                            isIdentifierStart(next.text[0]));
  }
  // iN and uN name an N-byte integer. Every i/u + digits word is reserved so
  // that out-of-range widths get a precise diagnostic.
  bool byteWidthType() const {
    // No fixed spelling is i or u followed by digits, so only literals can be.
    if (peek().string || peek().text.kind() != Tok::Literal) return false;

    const std::string &t = peek().text;
    return t.size() >= 2 && (t[0] == 'i' || t[0] == 'u') &&
           std::all_of(t.begin() + 1, t.end(),
                       [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
  }
  bool isType() const {
    return byteWidthType() || at(Tok::Byte) || at(Tok::Uint) || at(Tok::Uint8) || at(Tok::Uint16) ||
           at(Tok::Uint32) || at(Tok::Uint64) ||
           at(Tok::Int8) || at(Tok::Short) || at(Tok::Int16) ||
           at(Tok::Int) || at(Tok::Int32) || at(Tok::Long) || at(Tok::Int64) ||
           at(Tok::Float) || at(Tok::Double) || at(Tok::Bool) || at(Tok::Void) || at(Tok::Cstring) ||
           at(Tok::String) || at(Tok::Name) || at(Tok::Char) || at(Tok::Date) ||
           at(Tok::Time) || at(Tok::Datetime) || at(Tok::Big) || at(Tok::Ubig) || at(Tok::Tiny) ||
           at(Tok::Utiny) || at(Tok::Func) || nativeMathType() || markupKeyword() || fileKeyword() || taskKeyword();
  }
  // `task` is a type only where one can be, as `file` is: `task worker` or
  // `task& worker`, while `int task` still names a variable.
  bool taskKeyword() const {
    if (!at(Tok::Task) || taskContainer) return false;

    const auto &next = tokens.at(pos + 1);
    return !next.string && !next.text.empty() && (isIdentifierStart(next.text[0]) || next.text == Tok::Ampersand);
  }
  bool nativeMathType() const {
    const auto &token = tokens.at(pos);
    return !token.string && !token.character && nativeMath(token.text);
  }
  // `file` is a type only where one can be, as in `file log` or `file& log`:
  // it is still a usable name, as in `void* file`.
  bool fileKeyword() const {
    if (!at(Tok::File) || fileContainer) return false;

    const auto &next = tokens.at(pos + 1);
    return !next.string && !next.text.empty() && (isIdentifierStart(next.text[0]) || next.text == Tok::Ampersand);
  }
  // `markup` is a type only where one can be: it is still a usable name.
  bool markupKeyword() const {
    if (!at(Tok::Markup) || markupContainer) return false;

    const auto &next = tokens.at(pos + 1);
    return !next.string && !next.text.empty() &&
           isIdentifierStart(next.text[0]);
  }
  // char(...), date(...), time(...), and datetime(...) construct validated values.
  bool constructorType() const {
    const auto &next = tokens.at(pos + 1);
    return (at(Tok::Char) || at(Tok::Date) || at(Tok::Time) || at(Tok::Datetime) || nativeMathType()) &&
           !next.string && next.text == Tok::LeftParen;
  }
  Type type() {
    Type t = plainType();

    if (arraySuffix(pos)) requireFuncSignature(t, "an array of func");

    // `T [N]` is a fixed array of N elements and `T []` a dynamic array.
    // Further dimensions follow in index order: `T [5][10]` holds 5 arrays
    // of 10. `T [-][-]` is a sparse grid indexed by any int coordinates.
    if (arraySuffix(pos)) {
      struct Dimension {
        Token token;
        unsigned long long extent = 0, sizeHint = 0;
        bool signedIndex = false;
      };
      std::vector<Dimension> dimensions;

      while (arraySuffix(pos)) {
        Dimension d{pop()};

        if (accept(Tok::Minus)) d.signedIndex = true;
        else if (accept(Tok::Star)) d.sizeHint = Type::HugeHint;
        else if (!at(Tok::RightBracket)) {
          const auto count = pop();
          const bool hint = accept(Tok::Star);

          if (integerLiteralBase(count.text) != 10 ||
              count.text.find_first_not_of("0123456789") != std::string::npos)
            fail(count, hint ? "a size hint must be a decimal integer literal"
                             : "a fixed array's length must be a decimal integer literal");

          unsigned long long value = 0;

          try {
            value = std::stoull(count.text);
          } catch (const std::exception &) {
            fail(count, hint ? "size hint is too large" : "fixed array length is too large");
          }

          if (hint && (value == 0 || value == Type::HugeHint))
            fail(count, value ? "size hint is too large" : "a size hint needs at least one element; use T [] when there is no estimate");

          if (!hint && value == 0) fail(count, "a fixed array needs at least one element; use T [] for a dynamic array");

          (hint ? d.sizeHint : d.extent) = value;
        }

        expect(Tok::RightBracket);
        dimensions.push_back(d);
      }

      if (collectionWord()) fail(peek(), "arrays of collections are not supported yet");

      size_t signedCount = 0;

      for (const auto &d : dimensions) signedCount += d.signedIndex;

      if (signedCount) {
        if (signedCount != dimensions.size())
          for (const auto &d : dimensions)
            if (!d.signedIndex)
              fail(d.token, "a signed [-] dimension cannot be mixed with [N] or [] dimensions; "
                            "make every dimension [-]");

        return Type::grid(t, unsigned(dimensions.size()));
      }

      t = trackedElement(t);

      for (size_t i = dimensions.size(); i-- > 0;) {
        t = Type::array(t, dimensions[i].extent);
        t.sizeHint = dimensions[i].sizeHint;
      }

      return t;
    }

    // `K, V pair` names a pair keyed by K.
    if (at(Tok::Comma)) {
      const auto saved = pos;
      pop();

      if (declarationType() || isType()) {
        const auto value = plainType();

        if (collectionWord() && peek().text == Tok::Pair) {
          pop();
          return Type::collection(Type::Pair, trackedElement(value), t);
        }
      }

      pos = saved;
    }

    if ((at(Tok::Queue) || at(Tok::Stack) || at(Tok::Pair)) && arraySuffix(pos + 1))
      fail(peek(), "arrays of collections are not supported yet");

    // `T queue`, `T stack`, and `T pair` (keyed by name).
    if (collectionWord()) {
      const auto word = pop().text;

      if (word == Tok::Queue) return Type::collection(Type::Queue, trackedElement(t));
      if (word == Tok::Stack) return Type::collection(Type::Stack, trackedElement(t));

      return Type::collection(Type::Pair, trackedElement(t), Type::Name);
    }

    return t;
  }
  // A pointer to a heap value stored in a collection is a tracked alias.
  static Type trackedElement(Type t) {
    if (t.depth == 1 && heapValue(t.pointee())) t.role = Type::Alias;

    return t;
  }
  Type plainType() {
    Type t = baseType();

    while (accept(Tok::Star)) t = t.pointerTo();
    if (t.depth && t.base == Type::Func) fail(peek(), "pointers to func values are unsupported");

    if (t.depth && (t.base == Type::String || t.base == Type::Name ||
                    t.base == Type::Big || t.base == Type::UBig))
      fail(peek(), "pointers to managed text or big integers are unsupported");

    return t;
  }
  // `queue`, `stack`, or `pair` after an element type, followed by the
  // declared name or a reference marker; elsewhere they are ordinary names.
  bool collectionWord() const {
    if (!at(Tok::Queue) && !at(Tok::Stack) && !at(Tok::Pair)) return false;

    const auto &next = tokens.at(pos + 1);
    return !next.string && (next.text == Tok::Ampersand || isIdentifierStart(next.text[0]));
  }
  Type baseType() {
    std::string qualified;

    if (const auto length = typePath(pos, &qualified)) {
      Type t = containerNames.count("#@" + qualified) ? Type::Enum : Type::Record;
      t.record = qualified;
      identifier();

      for (size_t k = 1; k < length; ++k) pop();

      return t;
    }

    if (nativeMathType()) {
      Type t(Type::Record);
      t.record = pop().text;
      return t;
    }

    if (byteWidthType()) {
      const auto token = pop();
      const auto digits = token.text.substr(1);

      if (digits[0] == '0' || digits.size() > 2 || std::stoi(digits) > 64)
        fail(token, "integer byte width must be 1 to 64: '" + token.text + "'");

      return Type::integer(8 * unsigned(std::stoi(digits)), token.text[0] == 'u');
    }

    if (accept(Tok::Byte) || accept(Tok::Uint8))
      return Type::UInt8;

    if (accept(Tok::Uint16))
      return Type::UInt16;

    if (accept(Tok::Uint) || accept(Tok::Uint32))
      return Type::UInt32;

    if (accept(Tok::Uint64))
      return Type::UInt64;

    if (accept(Tok::Int8))
      return Type::Int8;

    if (accept(Tok::Short) || accept(Tok::Int16))
      return Type::Short;

    if (accept(Tok::Long) || accept(Tok::Int64))
      return Type::Long;

    if (accept(Tok::Float))
      return Type::Float;

    if (accept(Tok::Double))
      return Type::Double;

    if (accept(Tok::Int) || accept(Tok::Int32))
      return Type::Int;

    if (accept(Tok::Bool))
      return Type::Bool;

    if (markupKeyword()) {
      pop();
      return markupType();
    }

    if (fileKeyword()) {
      pop();
      return Type::File;
    }

    if (taskKeyword()) {
      pop();
      return Type::Task;
    }

    if (accept(Tok::String)) return Type::String;
    if (accept(Tok::Name)) return Type::Name;
    if (accept(Tok::Char)) return Type::Char;
    if (accept(Tok::Date)) return Type::Date;
    if (accept(Tok::Time)) return Type::Time;
    if (accept(Tok::Datetime)) return Type::DateTime;
    if (accept(Tok::Func)) return funcType();
    if (accept(Tok::Big)) return Type::Big;
    if (accept(Tok::Ubig)) return Type::UBig;
    if (accept(Tok::Tiny)) return Type::Tiny;
    if (accept(Tok::Utiny)) return Type::UTiny;

    if (accept(Tok::Cstring))
      return Type::Text;

    expect(Tok::Void);
    return Type::Void;
  }
  // After `func`: a signature ahead, as in `func int(float)` or `func (int)`.
  bool funcSignatureAhead() {
    if (at(Tok::LeftParen)) return true;
    if (!isType() && !namedType()) return false;

    const auto saved = pos;
    plainType();
    const bool found = at(Tok::LeftParen);
    pos = saved;
    return found;
  }
  // `func`, or `func result(parameter types)`. Parameters may be named.
  Type funcType() {
    Type t(Type::Func);

    if (!funcSignatureAhead()) return t;

    auto signature = std::make_shared<FuncSignature>();

    if (!at(Tok::LeftParen)) signature->result = type();

    expect(Tok::LeftParen);
    lines();

    if (!at(Tok::RightParen))
      do {
        lines();
        Type parameter = type();

        if (parameter == Type::Void) fail(peek(), "parameter cannot have void type");

        requireFuncSignature(parameter, "a func parameter");
        bool reference = accept(Tok::Ampersand);

        if (!at(Tok::Comma) && !at(Tok::RightParen) && !at(Tok::Newline)) identifier();

        signature->parameters.push_back({parameter, reference});
        lines();
      } while (accept(Tok::Comma));

    expect(Tok::RightParen);
    requireFuncSignature(signature->result, "a func result");
    t.signature = signature;
    return t;
  }
  void requireFuncSignature(const Type &t, const std::string &what) const {
    if (funcValue(t) && !t.signature)
      fail(peek(), what + " needs a signature, as in func int(float)");
  }
  // `[A, B.C]` lists before a declaration.
  std::vector<Tag> attributes() {
    std::vector<Tag> tags;

    while (accept(Tok::LeftBracket)) {
      do {
        lines();
        auto token = identifier();

        while (accept(Tok::Dot)) token.text += "." + identifier().text;

        tags.push_back({token});
        lines();
      } while (accept(Tok::Comma));

      expect(Tok::RightBracket);
      lines();
    }

    return tags;
  }
  // The parameter list of a function or lambda, from '(' through ')'.
  void parameterList(Function &f) {
    expect(Tok::LeftParen);
    lines();

    if (!at(Tok::RightParen))
      do {
        lines();

        if (!declarationType()) rejectMisspelledType(peek());

        if (!declarationType())
          fail(peek(),
               "parameter requires a supported explicit type (int8, short, "
               "int16, int, int32, long, int64, byte, uint, uint8, uint16, "
               "uint32, uint64, i1-i64, u1-u64, big, ubig, tiny, utiny, float, double, bool, "
               "string, name, char, date, time, "
               "datetime, cstring, func, or void*)");

        Type t = type();

        if (t == Type::Void)
          fail(peek(), "parameter cannot have void type");

        requireFuncSignature(t, "a func parameter");
        bool reference = accept(Tok::Ampersand);
        auto parameterName = identifier();
        parameterName.text = declareLocal(parameterName);
        f.params.push_back({parameterName, t, reference});
        lines();
      } while (accept(Tok::Comma));

    expect(Tok::RightParen);
  }
  // `@ result(parameters) { body }`, after the '@'.
  void lambdaExpression(Expr &e, bool blockBranch = false) {
    e.kind = ExprKind::Lambda;
    Type result = Type::Void;

    if (!blockBranch && !at(Tok::LeftParen)) result = type();

    requireFuncSignature(result, "a func result");
    Token name = e.token;
    name.text = Tok::Lambda;
    Function f{name, result, {}, {}, false, {}};
    f.usings = usings;
    hygiene.emplace_back();
    if (!blockBranch) parameterList(f);
    const bool outerOwner = returnsOwner;
    returnsOwner = false;
    // An assertion leaves its test, and a lambda body cannot do that.
    const auto outerTest = std::exchange(currentTest, "");
    ++lambdaDepth;
    const bool outerMarkup = std::exchange(returnsMarkup, markupValue(result));
    f.body = block();
    returnsMarkup = outerMarkup;
    --lambdaDepth;
    currentTest = outerTest;
    hygiene.pop_back();

    if (returnsOwner) fail(name, "a lambda cannot return ownership with return ::");

    returnsOwner = outerOwner;
    auto signature = std::make_shared<FuncSignature>();
    signature->result = result;

    for (const auto &p : f.params) signature->parameters.push_back({p.type, p.reference});

    e.castType = Type(Type::Func);
    e.castType.signature = signature;
    e.lambda = std::make_shared<Function>(std::move(f));
  }
  // `P{ body }` as a value starts a task: a thread of its own, given back as
  // a task handle.
  bool detachedAhead() const {
    return at(Tok::P) && punctuation(tokens.at(pos + 1), Tok::LeftBrace) && !pContainer;
  }
  // After the 'P': the body, as a lambda without parameters.
  void detachedTask(Expr &e) {
    e.kind = ExprKind::Detached;
    auto body = std::make_unique<Expr>();
    body->token = peek();
    lambdaExpression(*body, true);
    e.args.push_back(std::move(body));

    if (at(Tok::LeftBrace) || at(Tok::LeftParen))
      fail(e.token, "a task started with P{ } has one body; P{ }{ } with two or more branches is a statement that "
                    "runs them in parallel and waits for all of them");
  }
  static int precedence(const Token &t) {
    if (t.string)
      return 0;

    const std::string &s = t.text;

    if (s == "||")
      return 1;

    if (s == "&&")
      return 2;

    if (s == "|")
      return 3;

    if (s == "^")
      return 4;

    if (s == "&")
      return 5;

    if (s == "==" || s == "!=")
      return 6;

    if (s == "<" || s == ">" || s == "<=" || s == ">=")
      return 7;

    if (s == "<<" || s == ">>")
      return 8;

    if (s == "+" || s == "-")
      return 9;

    if (s == "*" || s == "/" || s == "%")
      return 10;

    return 0;
  }
  void parenthesized(std::unique_ptr<Expr> &e, bool stopAtPipe) {
    lines();

    std::string named;
    const auto namedLength = typePath(pos, &named);

    // `(Day)n` converts a value to the enum member that holds it.
    if ((isType() && !constructorType() && !typeMember()) ||
        (namedLength && tokens[pos + namedLength].text == Tok::Star) ||
        (namedLength && containerNames.count("#@" + named) && !tokens[pos + namedLength].string &&
         tokens[pos + namedLength].text == Tok::RightParen)) {
      e->kind = ExprKind::Cast;
      e->castType = type();
      lines();
      expect(Tok::RightParen);
      e->args.push_back(expression(11, stopAtPipe));
    } else {
      e = expression();
      lines();

      if (at(Tok::ColonColon) && e->kind == ExprKind::Member) {
        // `(a.Child :: b)` gives false instead of trapping on an ownership
        // cycle, and leaves b's owner as it was.
        auto move = std::make_unique<Expr>();
        move->kind = ExprKind::MoveCheck;
        move->token = pop();
        move->args.push_back(std::move(e));
        move->args.push_back(expression());
        e = std::move(move);
        lines();
      }

      if (at(Tok::PlusAssign) || at(Tok::MinusAssign) || at(Tok::PlusPlus) || at(Tok::MinusMinus)) {
        // `(s += x)` gives the inserted index; `(s--)` and `(s -= [i])`
        // give the removed element.
        auto mutation = std::make_unique<Expr>();
        mutation->kind = ExprKind::Mutation;
        mutation->token = pop();
        mutation->args.push_back(std::move(e));

        if (mutation->token.text == Tok::PlusAssign || mutation->token.text == Tok::MinusAssign)
          mutation->args.push_back(mutationOperand(mutation->token.text));

        e = std::move(mutation);
        lines();
      }

      expect(Tok::RightParen);
    }
  }
  void braceLiteral(Expr &literal) {
    // A literal: `{X: 1}` initializes a container's fields, `{1, 2}` lists
    // values, and `{"a" : 1}` lists keyed entries. A bare name before ':'
    // is a field-style entry (its value first, then the name as written);
    // the expected type decides what the literal builds.
    literal.kind = ExprKind::Initializer;
    lines();

    while (!at(Tok::RightBrace)) {
      // A word right before ':' is a field-style key, even a keyword such
      // as `name` in {name: "Bob"}.
      const auto &next = tokens.at(pos + 1);
      std::unique_ptr<Expr> first;

      if (!peek().string && isIdentifierStart(peek().text[0]) &&
          !next.string && next.text == Tok::Colon) {
        first = std::make_unique<Expr>();
        first->kind = ExprKind::Name;
        first->token = pop();
      } else
        first = expression();

      if (accept(Tok::Colon)) {
        auto entry = std::make_unique<Expr>();

        if (first->kind == ExprKind::Name) {
          entry->kind = ExprKind::FieldInitializer;
          entry->token = first->token;
          entry->args.push_back(expression());
          entry->args.push_back(std::move(first));
        } else {
          entry->kind = ExprKind::Entry;
          entry->token = first->token;
          entry->args.push_back(std::move(first));
          entry->args.push_back(expression());
        }

        literal.args.push_back(std::move(entry));
      } else
        literal.args.push_back(std::move(first));

      if (!at(Tok::RightBrace) && !at(Tok::Newline) && !at(Tok::Comma)) fail(peek(), "expected ',' or a new line between literal entries");

      accept(Tok::Comma);
      lines();
    }

    expect(Tok::RightBrace);
  }
  std::unique_ptr<Expr> primaryExpression(bool stopAtPipe, bool markupExpected) {
    auto e = std::make_unique<Expr>();
    e->token = peek();

    if (accept(Tok::MacroValue)) {
      // An expanded value macro: its argument bindings, then its result.
      e->kind = ExprKind::MacroValue;
      hygiene.emplace_back();

      while (!accept(Tok::MacroResult)) {
        e->bindings.push_back(simpleStatement());
        expect(Tok::Newline);
      }

      e->args.push_back(expression());
      lines();
      expect(Tok::MacroEnd);
      hygiene.pop_back();
    } else if (accept(Tok::Hash)) {
      e->kind = ExprKind::CompileTime;
      e->args.push_back(expression(11, stopAtPipe));

      if (e->args[0]->kind != ExprKind::Call)
        fail(e->token, "'#' requires an explicit function call, as in #Call(1); "
                       "a constant declaration requires an explicit type");
    } else if (accept(Tok::Minus) || accept(Tok::Bang) || accept(Tok::Tilde) || accept(Tok::Ampersand) || accept(Tok::Star)) {
      e->kind = ExprKind::Unary;
      e->args.push_back(expression(11, stopAtPipe));
    } else if (accept(Tok::Question)) {
      // `?p.q.Foo()` skips the call, arguments included, at the first invalid
      // link and reads as the result type's default.
      // `?a[i]` guards an index: a missing element skips a write and reads
      // as the element type's default. `?e..Foo()` skips a missing method.
      e->kind = ExprKind::Guard;
      e->args.push_back(expression(11, stopAtPipe));

      if (e->args[0]->kind != ExprKind::Index && e->args[0]->kind != ExprKind::DuckCall && e->args[0]->kind != ExprKind::Name &&
          e->args[0]->kind != ExprKind::Member && !(e->args[0]->kind == ExprKind::Call && e->args[0]->receiver))
        fail(e->token, "'?' guards an index such as ?a[i], a method call such as ?p.Foo(), "
                       "a duck call such as ?e..Foo(), or a scoped variable");
    } else if (at(Tok::At) && word(tokens.at(pos + 1)) && !punctuation(tokens.at(pos + 2), Tok::LeftParen) &&
               !punctuation(tokens.at(pos + 2), Tok::Star) && !punctuation(tokens.at(pos + 2), Tok::LeftBracket) &&
               !punctuation(tokens.at(pos + 2), Tok::Dot)) {
      // `@Name` asks whether that scope is active. A lambda's result type is
      // always followed by its parameter list.
      pop();
      e->kind = ExprKind::ScopeActive;
      e->token = identifier();
    } else if (at(Tok::LeftBrace) && (markupExpected || markupAhead())) {
      markupLiteral(*e, nullptr);
    } else if (detachedAhead()) {
      pop();
      detachedTask(*e);
    } else if (accept(Tok::At)) {
      lambdaExpression(*e);
    } else if (accept(Tok::LeftParen)) {
      parenthesized(e, stopAtPipe);
    } else if (accept(Tok::LeftBrace)) {
      braceLiteral(*e);
    } else if (peek().character) {
      e->kind = ExprKind::Char;
      pop();
    } else if (peek().string) {
      e->kind = ExprKind::Text;
      pop();
    } else if (typeMember()) {
      e->kind = ExprKind::TypeMember;
      e->castType = baseType();
      expect(Tok::Dot);
      e->token = identifier();

      // vec3.Identity(): a function of the type rather than a limit like int.Max.
      if (nativeMath(e->castType) && accept(Tok::LeftParen)) {
        e->kind = ExprKind::Call;
        e->token.text = e->castType.record + "." + e->token.text;
        e->castType = Type();
        e->args = callArguments();
      }
    } else if (constructorType()) {
      e->kind = ExprKind::Construct;
      e->castType = baseType();
      expect(Tok::LeftParen);
      e->args = callArguments();
    } else if (accept(Tok::Null)) {
      e->kind = ExprKind::Null;
    } else if (at(Tok::True) || at(Tok::False)) {
      e->kind = ExprKind::Bool;
      pop();
    } else if (std::isdigit(static_cast<unsigned char>(peek().text[0]))) {
      e->kind = integerLiteralBase(peek().text) != 10 ||
                        peek().text.find_first_of(".eE") == std::string::npos
                    ? ExprKind::Int : ExprKind::Real;
      pop();
    } else {
      e->token = identifier();
      resolveLocal(e->token);
      e->kind = ExprKind::Name;
    }

    return e;
  }
  void duckCallSuffix(std::unique_ptr<Expr> &e) {
    // Dynamic dispatch: `e..Foo()`, and the checks `e..?Foo()` (would
    // this call run?) and `e..?Foo` (does any method of that name exist?).
    auto call = std::make_unique<Expr>();
    call->kind = accept(Tok::Question) ? ExprKind::DuckCheck : ExprKind::DuckCall;
    call->token = identifier();
    call->receiver = std::move(e);

    if (accept(Tok::LeftParen)) call->args = callArguments();
    else if (call->kind == ExprKind::DuckCheck) call->kind = ExprKind::DuckExists;
    else fail(call->token, "a duck call needs an argument list, as in e..Foo()");

    e = std::move(call);
  }
  void indexSuffix(std::unique_ptr<Expr> &e) {
    auto index = std::make_unique<Expr>();
    index->kind = ExprKind::Index;
    index->token = pop();
    index->args.push_back(std::move(e));
    lines();
    index->args.push_back(expression());
    lines();
    expect(Tok::RightBracket);
    e = std::move(index);
  }
  // The closing parenthesis of `a (0.5)== b`, or 0 if none is ahead. The
  // operator touches the `)`, and a space before the `(` tells it from the
  // call in `f(0.5)== b`.
  size_t toleranceClose() const {
    if (pos == 0 || !at(Tok::LeftParen)) return 0;

    const auto &before = tokens[pos - 1], &open = tokens[pos];

    if (before.line == open.line && before.column + int(before.text.size()) == open.column) return 0;

    int depth = 0;

    for (size_t k = pos; k + 1 < tokens.size(); ++k) {
      const auto &token = tokens[k];

      if (token.string) continue;
      if (token.text == Tok::LeftParen) ++depth;

      if (token.text == Tok::RightParen && --depth == 0) {
        const auto &next = tokens[k + 1];
        const bool comparison = !next.string && (next.text == Tok::EqualEqual || next.text == Tok::NotEqual);
        return comparison && next.line == token.line && next.column == token.column + 1 ? k : 0;
      }
    }

    return 0;
  }
  void postfixChain(std::unique_ptr<Expr> &e) {
    std::string path = e->kind == ExprKind::Name ? e->token.text.str() : std::string();

    while (at(Tok::Dot) || (at(Tok::LeftParen) && !toleranceClose()) || at(Tok::LeftBracket) || at(Tok::DotDot)) {
      if (accept(Tok::DotDot)) {
        duckCallSuffix(e);
        path.clear();
        continue;
      }

      if (at(Tok::LeftBracket)) {
        indexSuffix(e);
        path.clear();
        continue;
      }

      if (accept(Tok::Dot)) {
        auto member = std::make_unique<Expr>();
        member->kind = ExprKind::Member;
        member->token = identifier();
        path += "." + member->token.text;
        member->args.push_back(std::move(e));
        e = std::move(member);
      } else {
        expect(Tok::LeftParen);

        if (e->kind == ExprKind::Call || e->kind == ExprKind::Invoke || e->kind == ExprKind::Index || e->kind == ExprKind::Lambda ||
            e->kind == ExprKind::DuckCall) {
          // The callee is itself a value, as in MakeAdder(1)(2).
          auto invoke = std::make_unique<Expr>();
          invoke->kind = ExprKind::Invoke;
          invoke->token = e->token;
          invoke->receiver = std::move(e);
          invoke->args = callArguments();
          e = std::move(invoke);
          path.clear();
          continue;
        }

        if (e->kind != ExprKind::Name && e->kind != ExprKind::Member) fail(e->token, "expected a function or method name");

        if (e->kind == ExprKind::Member) {
          e->receiver = std::move(e->args.front());
          e->args.clear();
        }

        e->kind = ExprKind::Call;
        e->token.text = path;
        e->args = callArguments();
        path.clear();
      }
    }
  }
  // A node for the operator token at the cursor, with its left operand.
  std::unique_ptr<Expr> operatorNode(ExprKind kind, std::unique_ptr<Expr> left) {
    auto node = std::make_unique<Expr>();
    node->token = pop();
    node->kind = kind;
    node->args.push_back(std::move(left));
    return node;
  }
  std::unique_ptr<Expr> expression(int minimum = 1, bool stopAtPipe = false) {
    const bool markupExpected = std::exchange(expectMarkup, false);

    if (auto use = macroUse(tokens, pos)) expandValueMacro(*use);

    auto e = primaryExpression(stopAtPipe, markupExpected);
    postfixChain(e);

    // In a conditional's true arm, the first unparenthesized pipe is
    // its separator. Parentheses and call arguments start fresh expressions.
    for (;;) {
      if (const auto close = toleranceClose(); close && precedence(tokens[close + 1]) >= minimum) {
        expect(Tok::LeftParen);
        auto tolerance = expression();
        expect(Tok::RightParen);
        auto parent = operatorNode(ExprKind::Binary, std::move(e));
        int p = precedence(parent->token);
        parent->args.push_back(expression(p + 1, stopAtPipe));
        parent->args.push_back(std::move(tolerance)); // A third operand marks a tolerance.
        e = std::move(parent);
        continue;
      }

      if (precedence(peek()) < minimum || (stopAtPipe && at(Tok::Pipe))) break;

      auto parent = operatorNode(ExprKind::Binary, std::move(e));
      int p = precedence(parent->token);
      parent->args.push_back(expression(p + 1, stopAtPipe));
      e = std::move(parent);
    }

    // A range binds looser than every operator, so 0...a.Len-1 needs no
    // parentheses; a bare pipe after it belongs to the enclosing form.
    if (minimum == 1 && at(Tok::Ellipsis)) {
      auto parent = operatorNode(ExprKind::Range, std::move(e));
      parent->args.push_back(expression(2, true));
      e = std::move(parent);
    }

    if (minimum == 1 && at(Tok::Question)) {
      auto parent = operatorNode(ExprKind::Conditional, std::move(e));
      parent->args.push_back(expression(1, true));
      expect(Tok::Pipe);
      parent->args.push_back(expression(1, stopAtPipe));
      e = std::move(parent);
    }

    return e;
  }
  // The braces of the block parsed last, for Function::bodyOpen/bodyClose.
  Token blockOpen{}, blockClose{};
  std::vector<std::unique_ptr<Stmt>> block() {
    lines();
    const auto open = expect(Tok::LeftBrace);
    lines();
    hygiene.emplace_back();
    std::vector<std::unique_ptr<Stmt>> result;

    while (!at(Tok::RightBrace)) {
      if (at(Tok::Eof)) expected("}");

      result.push_back(statement());

      if (!at(Tok::RightBrace) && !at(Tok::Newline)) fail(peek(), "expected newline between statements");

      lines();
    }

    hygiene.pop_back();
    blockClose = expect(Tok::RightBrace);
    blockOpen = open;
    return result;
  }
  using Statements = std::vector<std::unique_ptr<Stmt>>;
  // Builds the expression a piece of generated markup code reads or stores to.
  using Place = std::function<std::unique_ptr<Expr>(const Token &)>;
  static std::unique_ptr<Expr> localName(const Token &at, const std::string &local) {
    auto e = std::make_unique<Expr>();
    e->token = at;
    e->token.text = local;
    e->token.string = e->token.character = false;
    e->kind = ExprKind::Name;
    return e;
  }
  static std::unique_ptr<Expr> memberOf(std::unique_ptr<Expr> base, const Token &member) {
    auto e = std::make_unique<Expr>();
    e->token = member;
    e->kind = ExprKind::Member;
    e->args.push_back(std::move(base));
    return e;
  }
  static std::unique_ptr<Stmt> assignment(std::unique_ptr<Expr> target, std::unique_ptr<Expr> value, const Token &at) {
    auto s = std::make_unique<Stmt>();
    s->token = at;
    s->kind = StmtKind::Assign;
    s->target = std::move(target);
    s->expr = std::move(value);
    return s;
  }
  // `{` that opens a markup literal: a block's first content is a tag.
  bool markupAhead() const {
    if (!at(Tok::LeftBrace)) return false;

    size_t k = pos + 1;

    while (k < tokens.size() && punctuation(tokens[k], Tok::Newline)) ++k;

    return k + 1 < tokens.size() && punctuation(tokens[k], Tok::Less) && word(tokens[k + 1]);
  }
  bool closingTagAhead() const { return at(Tok::Less) && punctuation(tokens.at(pos + 1), Tok::Slash); }
  // A value written directly inside an element, and the `value` field it needs.
  bool literalContent(Type &type) {
    const auto &t = peek();

    if (t.string) {
      type = t.character ? Type(Type::Char) : Type(Type::String);
      return true;
    }

    if (at(Tok::True) || at(Tok::False)) {
      type = Type::Bool;
      return true;
    }

    const auto &digit = at(Tok::Minus) ? tokens.at(pos + 1) : t;

    if (digit.string || digit.text.empty() || !std::isdigit(static_cast<unsigned char>(digit.text[0]))) return false;

    type = integerLiteralBase(digit.text) != 10 || digit.text.find_first_of(".eE") == std::string::npos
               ? Type(Type::Int) : Type(Type::Double);
    return true;
  }
  // The content of `{ ... }` or `<Tag> ... </Tag>`, up to its closing token.
  // Returns whether any child elements or spliced markup were written.
  bool markupItems(Statements &out, const Place &holder, const std::string &owner, const Token &ownerToken,
                   const Place &self, Record *root) {
    bool children = false;

    for (lines(); !at(Tok::RightBrace) && !closingTagAhead(); lines()) {
      if (at(Tok::Eof)) fail(peek(), "unterminated markup: expected '}' or a closing tag");

      if (at(Tok::Less)) {
        markupElement(out, holder, root);
        children = true;
        continue;
      }

      Type literal;

      if (literalContent(literal)) {
        if (owner.empty()) fail(peek(), "a literal value belongs inside an element");

        const auto at = peek();
        auto value = expression(11);
        Token field = at;
        field.text = Tok::ImplicitValue;
        field.string = field.character = false;
        out.push_back(assignment(memberOf(self(at), field), std::move(value), at));
        Token tag = ownerToken;
        tag.text = owner;
        output->markupValues.push_back({tag, literal});
        continue;
      }

      // Markup splices in; any other value is this element's `value`. Which
      // one it is depends on the expression's type, known to semantic analysis.
      const auto at = peek();
      auto value = expression(11);
      auto splice = std::make_unique<Stmt>();
      splice->token = at;
      splice->kind = StmtKind::MarkupSplice;
      splice->target = holder(at);
      Token field = at;
      field.text = Tok::ImplicitValue;
      field.string = field.character = false;
      splice->alternate.push_back(assignment(owner.empty() ? nullptr : memberOf(self(at), field), std::move(value), at));
      out.push_back(std::move(splice));
      children = true;

      if (root) root->markupClosed = false;
    }

    return children;
  }
  void closingTag(const std::string &tag, const std::string &written) {
    expect(Tok::Less);
    expect(Tok::Slash);
    std::string closing;
    const auto closingAt = peek();
    const auto length = typePath(pos, &closing);

    if (!length || closing != tag)
      fail(closingAt, "expected closing tag </" + written + ">");

    pos += length;
    expect(Tok::Greater);
  }
  // `Name=value` pairs through the end of a tag. Values stop at any operator,
  // so `/>` is never read as a division: parenthesize an expression.
  void markupAttributes(Statements &out, const Place &self) {
    for (lines(); !at(Tok::Slash) && !at(Tok::Greater); lines()) {
      const auto name = identifier();
      expect(Tok::Assign);
      auto value = expression(11);
      out.push_back(assignment(memberOf(self(name), name), std::move(value), name));
    }
  }
  void markupElement(Statements &out, const Place &holder, Record *root) {
    const auto open = pop();
    std::string tag;
    const auto length = typePath(pos, &tag);

    if (!length) fail(peek(), "unknown component '" + peek().text + "'; a tag names a container type");

    const auto written = peek().text;
    auto tagToken = peek();
    pos += length;
    const auto local = "$element" + std::to_string(markupLocals++);
    auto group = std::make_unique<Stmt>();
    group->token = open;
    group->kind = StmtKind::Block;
    auto own = std::make_unique<Stmt>();
    own->token = tagToken;
    own->token.text = local;
    own->kind = StmtKind::Own;
    own->name = local;
    Type record = Type::Record;
    record.record = tag;
    own->type = record.pointerTo();
    group->body.push_back(std::move(own));
    const Place self = [local](const Token &at) { return localName(at, local); };
    markupAttributes(group->body, self);

    if (accept(Tok::Slash)) expect(Tok::Greater);
    else {
      expect(Tok::Greater);
      Token field = tagToken;
      field.text = Tok::Children;
      const Place children = [local, field](const Token &at) { return memberOf(localName(at, local), field); };
      tagToken.text = tag;

      if (markupItems(group->body, children, tag, tagToken, self, nullptr)) output->markupHosts.push_back(tagToken);

      closingTag(tag, written);
    }

    auto add = std::make_unique<Stmt>();
    add->token = open;
    add->kind = StmtKind::MarkupAdd;
    add->target = holder(open);
    add->expr = localName(open, local);
    group->body.push_back(std::move(add));
    out.push_back(std::move(group));

    if (root) ++root->markupChildren[tag];
  }
  // A `{ ... }` markup literal: evaluates to a new markup value. For a
  // container's `this{ }` block, root collects what the block declares.
  void markupLiteral(Expr &e, Record *root, const std::string &bare = "", const std::string &tag = "") {
    const auto start = peek();
    e.kind = ExprKind::MacroValue;
    e.token = start;
    const auto local = "$markup" + std::to_string(markupLocals++);
    auto let = std::make_unique<Stmt>();
    let->token = start;
    let->token.text = local;
    let->kind = StmtKind::Let;
    let->name = local;
    let->type = markupType();
    e.bindings.push_back(std::move(let));
    const Place holder = [local](const Token &at) { return localName(at, local); };
    expect(Tok::LeftBrace);
    lines();
    Token receiver = start;
    receiver.text = Tok::This;
    const Place self = [](const Token &at) { return localName(at, "this"); };

    // A block's root may be written out; it is always the container itself.
    if (root && at(Tok::Less) && tokens.at(pos + 1).text == bare && !punctuation(tokens.at(pos + 2), Tok::Dot)) {
      pop();
      const auto rootName = pop();
      markupAttributes(e.bindings, self);

      if (accept(Tok::Slash)) expect(Tok::Greater);
      else {
        expect(Tok::Greater);
        markupItems(e.bindings, holder, tag, rootName, self, root);
        closingTag(tag, bare);
      }

      lines();
    } else
      markupItems(e.bindings, holder, tag, receiver, self, root);

    expect(Tok::RightBrace);
    auto take = std::make_unique<Expr>();
    take->token = start;
    take->token.text = local;
    take->kind = ExprKind::MarkupTake;
    e.args.push_back(std::move(take));
  }
  // `this{ ... }` in an instance body: children built with each instance, by
  // a generated method called from the construction of a hidden field.
  void markupBlock(Program &program, Record &record, std::set<std::string> &members, const std::string &bare) {
    auto name = pop();

    if (!members.insert("$markup").second) fail(name, "container '" + record.name.text + "' already has a this{ } block");

    Function f{name, markupType(), {}, {}, true, {}};
    f.name.text = methodKey(record.name.text, "$markup");
    f.usings = usings;
    f.protectedAccess = true;
    Token self = name;
    self.text = Tok::This;
    Type type = Type::Record;
    type.record = record.name.text;
    f.params.push_back({self, type, true});
    record.markupClosed = true;
    auto result = std::make_unique<Stmt>();
    result->kind = StmtKind::Return;
    result->token = peek();
    result->expr = std::make_unique<Expr>();
    markupLiteral(*result->expr, &record, bare, record.name.text);
    f.body.push_back(std::move(result));
    program.push_back(std::move(f));
    auto call = std::make_unique<Expr>();
    call->kind = ExprKind::Call;
    call->token = name;
    call->token.text = Tok::SelfMarkup;
    call->receiver = localName(name, "$self");
    Token field = name;
    field.text = Tok::Children;
    record.fields.push_back({field, markupType(), false, false, false, std::shared_ptr<Expr>(std::move(call))});
    record.fields.back().usings = usings;
  }
  // Whether `T.Name(` here is an assertion. A program's own container named
  // T keeps its members, so only the assertion names are claimed.
  bool assertionAhead() const {
    if (!at(Tok::T) || !punctuation(tokens.at(pos + 1), Tok::Dot) || !word(tokens.at(pos + 2)) ||
        !punctuation(tokens.at(pos + 3), Tok::LeftParen))
      return false;

    const bool container = tContainer;

    if (assertionNames().count(tokens[pos + 2].text)) return !currentTest.empty() || !container;

    return !currentTest.empty() && !container;
  }
  static const std::map<std::string, std::pair<size_t, size_t>> &assertionNames() {
    static const std::map<std::string, std::pair<size_t, size_t>> names{
        {"AreEqual", {2, 2}}, {"AreNotEqual", {2, 2}}, {"IsTrue", {1, 1}}, {"IsFalse", {1, 1}}, {"Fail", {0, 1}}};
    return names;
  }
  std::unique_ptr<Expr> syntheticExpr(const Token &start, ExprKind kind, const std::string &text) {
    auto e = std::make_unique<Expr>();
    e->token = start;
    e->token.text = text;
    e->kind = kind;
    return e;
  }
  std::unique_ptr<Expr> syntheticText(const Token &start, const std::string &value) {
    auto e = syntheticExpr(start, ExprKind::Text, value);
    e->token.string = true;
    return e;
  }
  std::unique_ptr<Expr> syntheticNot(const Token &start, std::unique_ptr<Expr> operand) {
    auto e = syntheticExpr(start, ExprKind::Unary, "!");
    e->args.push_back(std::move(operand));
    return e;
  }
  void assertionFailureStep(std::vector<std::unique_ptr<Stmt>> &failure, const Token &start, const std::string &op, std::unique_ptr<Expr> first, std::unique_ptr<Expr> second = nullptr) {
    auto call = syntheticExpr(start, ExprKind::Call, op);
    call->args.push_back(std::move(first));

    if (second) call->args.push_back(std::move(second));

    auto s = std::make_unique<Stmt>();
    s->token = start;
    s->kind = StmtKind::Expr;
    s->expr = std::move(call);
    failure.push_back(std::move(s));
  }
  std::unique_ptr<Expr> comparisonAssertion(const Token &start, const Token &check, std::vector<std::unique_ptr<Expr>> &arguments, std::vector<std::unique_ptr<Stmt>> &statements, std::vector<std::unique_ptr<Stmt>> &failure) {
    std::unique_ptr<Expr> condition;
    // A literal stays in place, so the comparison type-checks exactly as a
    // written `a == 5` would and `null` keeps the other operand's type.
    struct Operand {
      ExprKind kind;
      Token token;
    };
    auto operand = [&](std::unique_ptr<Expr> value, const std::string &local) {
      switch (value->kind) {
      case ExprKind::Int:
      case ExprKind::Real:
      case ExprKind::Bool:
      case ExprKind::Null:
      case ExprKind::Text: return Operand{value->kind, value->token};
      default: break;
      }

      auto let = std::make_unique<Stmt>();
      let->token = start;
      let->token.text = local;
      let->kind = StmtKind::Let;
      let->name = local;
      let->expr = std::move(value);
      statements.push_back(std::move(let));
      return Operand{ExprKind::Name, syntheticExpr(start, ExprKind::Name, local)->token};
    };
    auto read = [](const Operand &o) {
      auto e = std::make_unique<Expr>();
      e->kind = o.kind;
      e->token = o.token;
      return e;
    };
    const auto actual = operand(std::move(arguments[0]), "$actual");
    const auto expected = operand(std::move(arguments[1]), "$expected");
    auto equal = syntheticExpr(start, ExprKind::Binary, "==");
    equal->args.push_back(read(actual));
    equal->args.push_back(read(expected));
    const bool wantEqual = check.text == Tok::AreEqual;
    condition = wantEqual ? syntheticNot(start, std::move(equal)) : std::move(equal);
    auto show = [&](const char *title, const Operand &o) {
      if (o.kind != ExprKind::Null) assertionFailureStep(failure, start, "$T.show", read(o), syntheticText(start, title));
    };

    if (wantEqual) {
      show("  actual:   ", actual);
      show("  expected: ", expected);
    } else
      show("  both:     ", actual);

    return condition;
  }
  std::unique_ptr<Stmt> assertionCheck(const Token &start, std::unique_ptr<Expr> condition, std::vector<std::unique_ptr<Stmt>> failure, std::vector<std::unique_ptr<Stmt>> statements) {
    auto leave = std::make_unique<Stmt>();
    leave->token = start;
    leave->kind = StmtKind::Return;
    failure.push_back(std::move(leave));
    auto test = std::make_unique<Stmt>();
    test->token = start;
    test->kind = StmtKind::If;
    test->expr = std::move(condition);
    test->body = std::move(failure);

    if (statements.empty()) return test;

    statements.push_back(std::move(test));
    auto group = std::make_unique<Stmt>();
    group->token = start;
    group->kind = StmtKind::Block;
    group->body = std::move(statements);
    return group;
  }
  // An assertion lowers to ordinary statements: evaluate its operands once,
  // test them, and on failure report and return. Returning runs the test's
  // normal scope cleanup, so a failed test still releases what it holds.
  std::unique_ptr<Stmt> assertion() {
    const auto start = pop();
    expect(Tok::Dot);
    const auto check = identifier();
    const auto label = "T." + check.text;

    if (currentTest.empty())
      fail(start, label + " is available only directly inside a test in a T{ } block");

    const auto arity = assertionNames().find(check.text);

    if (arity == assertionNames().end())
      fail(check, "unknown assertion '" + label + "'; use AreEqual, AreNotEqual, IsTrue, IsFalse, or Fail");

    expect(Tok::LeftParen);
    auto arguments = callArguments();

    if (arguments.size() < arity->second.first || arguments.size() > arity->second.second)
      fail(start, label + " expects " + std::to_string(arity->second.second) + " argument" +
                      (arity->second.second == 1 ? "" : "s"));

    std::vector<std::unique_ptr<Stmt>> failure;
    assertionFailureStep(failure, start, "$T.fail", syntheticText(start, where(start) + ": test " + currentTest + ": " + label +
                              (check.text == Tok::Fail ? "" : " failed")));
    std::vector<std::unique_ptr<Stmt>> statements;
    std::unique_ptr<Expr> condition;

    if (arity->second.second == 2) {
      condition = comparisonAssertion(start, check, arguments, statements, failure);
    } else if (check.text == Tok::Fail) {
      // Conditional in form only: statements after T.Fail stay reachable.
      condition = syntheticExpr(start, ExprKind::Bool, "true");

      if (!arguments.empty()) assertionFailureStep(failure, start, "$T.show", std::move(arguments[0]), syntheticText(start, "  message:  "));
    } else
      condition = check.text == Tok::IsTrue ? syntheticNot(start, std::move(arguments[0])) : std::move(arguments[0]);

    return assertionCheck(start, std::move(condition), std::move(failure), std::move(statements));
  }
  bool scopeMarkAhead() const { return at(Tok::LeftBracket) && punctuation(tokens.at(pos + 1), Tok::At); }
  // `[@Name]` before a field or loop: the scope's name.
  Token scopeMark() {
    expect(Tok::LeftBracket);
    expect(Tok::At);
    auto scope = identifier();
    expect(Tok::RightBracket);
    lines();
    return scope;
  }
  // A marked field stores a slot; its value lives in the scope's arena.
  template <class Field>
  void markScoped(Field &field, const Token &mark, Type &stored, bool initialized) {
    if (initialized)
      fail(mark, "a field in scope '" + mark.text + "' starts at its type's default each time the scope starts; "
                 "assign it after @start instead of giving it an initializer");

    if (stored.depth) fail(mark, "a scoped field holds a value, not a pointer");

    field.scope = mark.text;
    field.scopedType = stored;
    stored = scopeSlotType();
  }
  bool scopeStatementAhead() const {
    return at(Tok::At) && tokens.at(pos + 2).line == peek().line &&
           (tokens.at(pos + 1).text == Tok::Start || tokens.at(pos + 1).text == Tok::End) && !tokens.at(pos + 1).string &&
           word(tokens.at(pos + 2));
  }
  std::unique_ptr<Stmt> scopeStatement() {
    auto s = std::make_unique<Stmt>();
    s->token = pop();
    const bool starting = pop().text == Tok::Start;
    s->kind = starting ? StmtKind::ScopeStart : StmtKind::ScopeEnd;

    if (!starting && lambdaDepth)
      fail(s->token, "@end leaves the functions up to its [@Name] loop, which a lambda body cannot do");

    s->name = identifier().text;

    if (!at(Tok::Newline) && !at(Tok::RightBrace)) badStatementEnd();

    return s;
  }
  // `[@Name]` before a loop: each iteration's body runs inside the scope.
  std::unique_ptr<Stmt> scopedLoop() {
    const auto mark = scopeMark();

    if (!at(Tok::While) && !at(Tok::For) && !at(Tok::Do))
      fail(mark, "a scope mark in a function body applies to a loop");

    auto loop = statement();
    auto body = std::make_unique<Stmt>();
    body->token = mark;
    body->kind = StmtKind::ScopedBody;
    body->name = mark.text;
    body->body = std::move(loop->body);
    loop->body.clear();
    loop->body.push_back(std::move(body));
    return loop;
  }
  std::unique_ptr<Stmt> doStatement(std::unique_ptr<Stmt> s) {
    s->kind = StmtKind::Do;
    s->body = controlBody();
    // A `while` on the body's last line is its condition. One on a later
    // line is too, unless it has a block of its own: that is the next
    // statement, and this `do` has no condition.
    const auto end = pos;
    const bool sameLine = at(Tok::While);
    lines();

    if (accept(Tok::While)) {
      auto condition = expression();
      const bool terminated = at(Tok::Newline) || at(Tok::RightBrace);

      if (sameLine && !terminated) fail(peek(), "expected terminator after do-while condition");

      const auto after = pos;
      lines();
      const bool ownBlock = at(Tok::LeftBrace);
      pos = after;

      if (terminated && (sameLine || !ownBlock)) {
        s->expr = std::move(condition);
        return s;
      }
    }

    pos = end;
    return s;
  }
  std::unique_ptr<Stmt> conditionalStatement(std::unique_ptr<Stmt> s) {
    s->kind = s->token.text == Tok::If ? StmtKind::If : StmtKind::While;
    s->expr = expression();
    s->body = controlBody();

    if (s->kind == StmtKind::If) {
      auto saved = pos;
      lines();

      if (accept(Tok::Else)) {
        lines();

        if (at(Tok::If))
          s->alternate.push_back(statement());
        else
          s->alternate = controlBody();
      } else
        pos = saved;
    }

    return s;
  }
  // A single P(...) remains an ordinary function call.
  bool parallelAhead() const {
    if (!at(Tok::P)) return false;
    size_t k = pos + 1;
    if (punctuation(tokens.at(k), Tok::LeftBrace)) return true;
    if (!punctuation(tokens.at(k), Tok::LeftParen) || !skipToCloseParen(k)) return false;
    return punctuation(tokens.at(k + 1), Tok::LeftParen) || punctuation(tokens.at(k + 1), Tok::LeftBrace);
  }
  std::unique_ptr<Stmt> parallelGroup() {
    auto group = std::make_unique<Stmt>();
    group->token = pop();
    group->kind = StmtKind::Parallel;
    do {
      if (at(Tok::LeftBrace)) {
        auto branch = std::make_unique<Expr>();
        branch->token = peek();
        lambdaExpression(*branch, true);
        group->labels.push_back(std::move(branch));
      } else {
        expect(Tok::LeftParen);
        lines();
        group->labels.push_back(expression());
        lines();
        expect(Tok::RightParen);
      }
    } while (at(Tok::LeftBrace) || at(Tok::LeftParen));
    // A lone `P{ }` starts a task nobody holds: the program waits for it at exit.
    if (group->labels.size() == 1) {
      auto task = std::make_unique<Expr>();
      task->token = group->token;
      task->kind = ExprKind::Detached;
      task->args.push_back(std::move(group->labels[0]));
      group->labels.clear();
      group->kind = StmtKind::Expr;
      group->expr = std::move(task);
    }
    return group;
  }
  std::unique_ptr<Stmt> statement() {
    expandStatementMacro();

    if (scopeMarkAhead()) return scopedLoop();
    if (scopeStatementAhead()) return scopeStatement();
    if (at(Tok::Switch)) return switchStatement();
    if (parallelAhead()) return parallelGroup();

    if (at(Tok::Macro) && word(tokens.at(pos + 1)) && punctuation(tokens.at(pos + 2), Tok::LeftParen))
      fail(peek(), "a macro cannot be declared inside a function; declare it at file scope or in a container");

    if (at(Tok::Case)) {
      const auto &next = tokens.at(pos + 1);

      if (next.string || next.text == Tok::Minus || word(next) || std::isdigit(static_cast<unsigned char>(next.text[0])))
        fail(peek(), "'case' is valid only directly inside a Switch body");
    }

    if (assertionAhead()) {
      auto check = assertion();

      if (!at(Tok::Newline) && !at(Tok::RightBrace)) badStatementEnd();

      return check;
    }

    auto s = std::make_unique<Stmt>();
    s->token = peek();

    if (at(Tok::LeftBrace)) {
      s->kind = StmtKind::Block;
      s->body = block();
      return s;
    }

    if (at(Tok::Dollar) && punctuation(tokens.at(pos + 1), Tok::LeftBrace)) {
      pop();
      s->kind = StmtKind::Ordered;
      s->body = block();
      return s;
    }

    if (accept(Tok::Do)) return doStatement(std::move(s));

    if (accept(Tok::For)) {
      hygiene.emplace_back();
      forClause(*s);
      hygiene.pop_back();
      return s;
    }

    if (accept(Tok::If) || accept(Tok::While)) return conditionalStatement(std::move(s));

    if (accept(Tok::Break) || accept(Tok::Continue))
      s->kind = s->token.text == Tok::Break ? StmtKind::Break : StmtKind::Continue;
    else if (accept(Tok::Return)) {
      s->kind = StmtKind::Return;

      if (accept(Tok::ColonColon)) {
        returnsOwner = true;
        s->kind = StmtKind::ReturnOwner;
      }

      if (!at(Tok::Newline) && !at(Tok::RightBrace)) {
        expectMarkup = returnsMarkup;
        s->expr = expression();
      }
    } else {
      const auto start = pos;
      s = simpleStatement();

      // Two words in a row: `itn a = 1` or `Foo f()` with an unknown first word.
      if (pos == start + 1 && isWord(tokens[start]) && !tokens[start].string && isWord(peek())) {
        rejectMisspelledType(tokens[start]);
        fail(tokens[start], "unknown type '" + tokens[start].text + "' (syntax may not be supported yet)");
      }
    }

    if (!at(Tok::Newline) && !at(Tok::RightBrace)) badStatementEnd(" (syntax may not be supported yet)");

    return s;
  }
  // One clause of a for loop and everything chained after it. `h | inner`
  // nests inner in h; `h | {after} inner` runs the body after the inner
  // loop and `h {before} | inner` before it. Each outer body shares one
  // statement list with its inner loop.
  void forClause(Stmt &s) {
    if (!forEachHeader(s)) {
      s.kind = StmtKind::For;

      if (!at(Tok::Semicolon))
        s.initializer = simpleStatement();

      expect(Tok::Semicolon);

      if (!at(Tok::Semicolon))
        s.expr = expression();

      expect(Tok::Semicolon);

      if (!at(Tok::LeftBrace) && !at(Tok::Newline) && !at(Tok::Pipe))
        s.step = simpleStatement(true);

      if (s.step && (s.step->kind == StmtKind::Let || s.step->kind == StmtKind::Own))
        fail(s.step->token, "for step cannot declare a variable");
    }

    std::vector<std::unique_ptr<Stmt>> before, after;

    if (accept(Tok::Pipe)) {
      lines();

      if (at(Tok::LeftBrace)) {
        after = block();
        lines();
      }
    } else {
      before = controlBody();
      const auto end = pos;
      lines();

      if (!accept(Tok::Pipe)) {
        pos = end;
        s.body = std::move(before);
        return;
      }

      lines();

      if (at(Tok::LeftBrace)) fail(peek(), "an outer loop takes one body, before or after its inner loop");
    }

    auto inner = std::make_unique<Stmt>();
    inner->token = peek();
    forClause(*inner);
    s.body = std::move(before);
    s.body.push_back(std::move(inner));

    for (auto &statement : after) s.body.push_back(std::move(statement));
  }
  // `for x : c`, `for -x : c`, or `for k, v : c`, after the `for`. A counted
  // `for i : 0; ...` starts the same way: it is told apart by its ';', and
  // the position is restored for the counted form.
  bool forEachHeader(Stmt &s) {
    const auto start = pos;
    const bool reverse = accept(Tok::Minus);
    auto first = peek();
    const bool word = !first.string && !first.text.empty() &&
                      isIdentifierStart(first.text[0]);

    if (!word || at(Tok::Eof) || (tokens[pos + 1].text != Tok::Colon && tokens[pos + 1].text != Tok::Comma) ||
        tokens[pos + 1].string) {
      pos = start;
      return false;
    }

    auto valueName = identifier();
    std::optional<Token> keyName;

    if (accept(Tok::Comma)) {
      keyName = valueName;
      valueName = identifier();
    }

    expect(Tok::Colon);
    auto collection = expression(1, true);

    if (at(Tok::Semicolon) && !reverse && !keyName) {
      pos = start;
      return false;
    }

    // Declared after the collection, which still sees the outer names.
    s.name = declareLocal(valueName);

    if (keyName) s.key = declareLocal(*keyName);

    if (!at(Tok::LeftBrace) && !at(Tok::Newline) && !at(Tok::Pipe))
      fail(peek(), "expected '{' or a newline after the collection of a for loop");

    if (collection->kind == ExprKind::Range && reverse)
      fail(collection->token, "a range already has a direction; write the high endpoint first, as 5...0");

    if (collection->kind == ExprKind::Range && !s.key.empty())
      fail(collection->token, "a range has no keys; write for i : a...b");

    s.kind = collection->kind == ExprKind::Range ? StmtKind::RangeFor : StmtKind::ForEach;
    s.reverse = reverse;
    s.expr = std::move(collection);
    return true;
  }
  std::vector<std::unique_ptr<Stmt>> controlBody() {
    lines();

    if (at(Tok::LeftBrace))
      return block();

    std::vector<std::unique_ptr<Stmt>> body;
    body.push_back(statement());
    return body;
  }
  // The arguments of a call after its '(' has been read, through ')'.
  std::vector<std::unique_ptr<Expr>> callArguments() {
    std::vector<std::unique_ptr<Expr>> arguments;
    lines();

    if (!at(Tok::RightParen))
      do {
        lines();
        arguments.push_back(expression());
        lines();
      } while (accept(Tok::Comma));

    expect(Tok::RightParen);
    return arguments;
  }
  // The right side of += or -=: `key : value` adds an entry to a pair, and
  // `[i]` or `[key]` after -= names the element to remove.
  std::unique_ptr<Expr> mutationOperand(const TokenText &op, bool stopAtPipe = false) {
    if (op == Tok::MinusAssign && at(Tok::LeftBracket)) {
      auto removal = std::make_unique<Expr>();
      removal->kind = ExprKind::Removal;
      removal->token = pop();
      lines();
      removal->args.push_back(expression());
      lines();
      expect(Tok::RightBracket);

      while (at(Tok::LeftBracket)) {
        pop();
        lines();
        removal->args.push_back(expression());
        lines();
        expect(Tok::RightBracket);
      }

      return removal;
    }

    auto value = expression(1, stopAtPipe);

    if (op == Tok::PlusAssign && at(Tok::Colon)) {
      auto entry = std::make_unique<Expr>();
      entry->kind = ExprKind::Entry;
      entry->token = pop();
      entry->args.push_back(std::move(value));
      entry->args.push_back(expression(1, stopAtPipe));
      return entry;
    }

    return value;
  }
  // stopAtPipe leaves an unparenthesized `|` for a chained for loop.
  std::unique_ptr<Stmt> simpleStatement(bool stopAtPipe = false) {
    auto s = std::make_unique<Stmt>();
    s->token = peek();

    // A leading # can mark either a typed constant or a call expression.
    if (accept(Tok::Hash)) {
      s->constant = declarationType();

      if (!s->constant) --pos;
    }

    if (s->constant && !declarationType())
      fail(peek(), "constant declaration requires an explicit type");

    if (declarationType()) {
      s->kind = StmtKind::Let;
      s->type = type();

      if (s->type == Type::Void)
        fail(s->token, "variable cannot have void type");

      const auto declaredName = identifier();
      s->name = declaredName.text;

      if (accept(Tok::LeftParen)) {
        // `Gun g(5)` and `Gun* p(5)` construct with arguments, like `Gun(5)`.
        auto arguments = callArguments();
        auto construction = [&](const Type &record) {
          auto call = std::make_unique<Expr>();
          call->kind = ExprKind::Call;
          call->token = s->token;
          call->token.text = record.record;
          call->castType = record; // Already resolved; names the constructed type.
          call->args = std::move(arguments);
          return call;
        };

        if (recordValue(s->type)) {
          if (!arguments.empty()) s->expr = construction(s->type);

          s->name = declareLocal(declaredName);
          return s;
        }

        if (s->type.depth != 1 || !heapValue(s->type.pointee()))
          fail(s->token, "heap construction requires a typed pointer");

        s->kind = StmtKind::Own;

        if (!arguments.empty()) {
          if (!recordValue(s->type.pointee()))
            fail(s->token, "heap construction arguments require a container type");

          s->expr = construction(s->type.pointee());
        }
      } else if (s->constant) {
        if (!accept(Tok::Assign))
          fail(peek(), "constant '" + s->name + "' requires an initializer, as in #int age = 5");

        s->expr = expression();
      } else if (accept(Tok::Assign)) {
        expectMarkup = markupValue(s->type);
        s->expr = expression();
      }

      // Declared after the initializer, which still sees the outer names.
      s->name = declareLocal(declaredName);
    } else {
      unreachableNestedType();
      auto target = expression(1, stopAtPipe);

      if (at(Tok::ColonColon) && target->kind == ExprKind::Member) {
        pop();
        s->kind = StmtKind::MoveInto;
        s->target = std::move(target);
        s->expr = expression();
      } else if (at(Tok::Colon) || at(Tok::ColonColon)) {
        if (target->kind != ExprKind::Name) fail(target->token, "expected local name");

        s->kind = pop().text == Tok::ColonColon ? StmtKind::Own : StmtKind::Let;
        s->expr = expression();
        s->name = declareLocal(target->token);
      } else if (at(Tok::Assign) || at(Tok::PlusPlus) || at(Tok::MinusMinus) || at(Tok::PlusAssign) || at(Tok::MinusAssign) ||
                 at(Tok::StarAssign) || at(Tok::SlashAssign) || at(Tok::PercentAssign) || at(Tok::AmpersandAssign) || at(Tok::PipeAssign) ||
                 at(Tok::CaretAssign) || at(Tok::ShiftLeftAssign) || at(Tok::ShiftRightAssign)) {
        auto op = pop().text;
        s->kind = assignmentKind(op.kind());
        s->target = std::move(target);

        if (op == Tok::PlusAssign || op == Tok::MinusAssign) s->expr = mutationOperand(op, stopAtPipe);
        else if (op != Tok::PlusPlus && op != Tok::MinusMinus) s->expr = expression(1, stopAtPipe);
      } else {
        s->kind = StmtKind::Expr;
        s->expr = std::move(target);
      }
    }

    return s;
  }

  Function function(Token name, Type result, bool external, const std::string &nativeName,
                    const std::string &receiver = "") {
    Function f{std::move(name), result, {}, {}, !receiver.empty(), {}};
    f.usings = usings;

    if (f.method) {
      Type self = Type::Record;
      self.record = receiver;
      Token token = f.name;
      token.text = Tok::This;
      f.params.push_back({token, self, true});
    }

    requireFuncSignature(f.result, "a func result");
    parameterList(f);

    if (external) {
      f.nativeSymbol = nativeName;

      if (accept(Tok::Assign)) {
        if (!peek().string || peek().character || peek().text.empty())
          fail(peek(), "native symbol must be a nonempty string literal");

        f.nativeSymbol = pop().text;
      }

      if (at(Tok::LeftBrace))
        fail(peek(), "native declaration cannot have a body");
    } else {
      returnsOwner = false;
      const bool outerMarkup = std::exchange(returnsMarkup, markupValue(f.result));
      f.body = block();
      f.bodyOpen = blockOpen;
      f.bodyClose = blockClose;
      returnsMarkup = outerMarkup;

      if (returnsOwner) {
        if (f.result.depth != 1 || f.result == Type::Pointer)
          fail(f.name, "return :: requires a typed heap pointer result");

        f.result.role = Type::Owner;
      }
    }

    return f;
  }
  // Moves k from a '(' to its matching ')'; false if the input ends first.
  bool skipToCloseParen(size_t &k) const {
    for (int depth = 0;; ++k) {
      const auto &t = tokens.at(k);

      if (punctuation(t, Tok::Eof)) return false;

      if (punctuation(t, Tok::LeftParen)) ++depth;
      else if (punctuation(t, Tok::RightParen) && --depth == 0) return true;
    }
  }
  // After a pointer-typed member name: `Item* Child()` with no body is an
  // owning field, while `Item* Get() { ... }` is a method.
  bool owningFieldAhead(const Type &t) const {
    if (t.depth != 1 || !heapValue(t.pointee())) return false;

    auto punct = [&](size_t k, auto text) { return !tokens.at(k).string && tokens.at(k).text == text; };
    size_t k = pos;

    if (!skipToCloseParen(k)) return false;
    for (++k; punct(k, Tok::Newline); ++k) {}

    return !punct(k, Tok::LeftBrace);
  }
  // A '+' (public, the default) or '-' (protected) prefix must touch the
  // member it modifies, as in -int Cash or -Helper().
  bool accessModifier() {
    if (!at(Tok::Plus) && !at(Tok::Minus)) return false;

    const auto sign = pop();
    const auto &next = peek();

    if (next.string || next.text == Tok::Newline || next.text == Tok::Eof ||
        next.source != sign.source || next.line != sign.line || next.column != sign.column + 1)
      fail(sign, "access modifier '" + sign.text +
                     "' must be written directly before the member, without spaces");

    return sign.text == Tok::Minus;
  }
  void destructorDeclaration(Program &program, std::set<std::string> &members, const std::string &bare, const Token &tag, const std::vector<Tag> &tags, const Token &tagged, bool isProtected, const Token &modifier) {
    if (!tags.empty()) fail(tagged, "attributes apply to fields and methods, not destructors");

    const auto tilde = pop();
    auto name = identifier();

    if (name.text != bare || tilde.line != name.line || tilde.column + 1 != name.column)
      fail(tilde, "a destructor is written ~" + bare + "()");

    if (isProtected) fail(modifier, "a destructor cannot be protected");
    if (!members.insert("~").second) fail(name, "container '" + tag.text + "' already declares a destructor");

    name.text = destructorKey(tag.text);
    program.push_back(function(std::move(name), Type::Void, false, "", tag.text));

    if (program.back().params.size() != 1) fail(tilde, "a destructor takes no parameters");
  }
  void constructorDeclaration(Program &program, std::set<std::string> &members, const Token &tag, const Token &name, bool typed, const std::vector<Tag> &tags, const Token &tagged, bool isProtected) {
    if (typed) fail(name, "a constructor has no result type");
    if (!tags.empty()) fail(tagged, "attributes apply to fields and methods, not constructors");

    auto constructor = function(name, Type::Void, false, "", tag.text);
    const std::vector<Parameter> params(constructor.params.begin() + 1, constructor.params.end());
    constructor.name.text = constructorKey(tag.text, params, isProtected);

    if (!members.insert(constructor.name.text).second)
      fail(name, "container '" + tag.text + "' already declares a " +
                     (isProtected ? "partial " : "") + "constructor with these parameter types");

    program.push_back(std::move(constructor));
  }
  void containerField(Record &record, const Token &name, Type t, bool typed, bool isProtected, std::vector<Tag> tags, const std::optional<Token> &scoped) {
    if (!typed && isWord(peek())) rejectMisspelledType(name);

    if (!typed && (at(Tok::Colon) || enumBlock()))
      fail(name, "an enum belongs at file scope or in a static #{ } block, not in a container's instance body");

    if (!typed || t == Type::Void)
      fail(name, "container fields require a known value type");

    std::shared_ptr<Expr> initializer;

    if (at(Tok::LeftParen)) {
      // `Item* Child()` is an owning field built with its container.
      if (!recordValue(t.pointee()))
        fail(name, "field '" + name.text + "' cannot be constructed; write " + typeName(t.pointee()) + "* " +
                       name.text + " :: value");

      initializer = std::make_shared<Expr>();
      initializer->kind = ExprKind::Call;
      initializer->token = name;
      initializer->token.text = t.record;
      initializer->castType = t.pointee();
      expect(Tok::LeftParen);
      initializer->args = callArguments();
      t.role = Type::Owner;
    } else if (accept(Tok::ColonColon)) {
      // `Item* Child :: null` is an owning field that starts empty.
      if (t.depth != 1 || !heapValue(t.pointee()))
        fail(name, "an owning field needs a typed heap pointer");

      t.role = Type::Owner;

      if (!accept(Tok::Null)) initializer = expression();
    } else {
      if (accept(Tok::Assign)) initializer = expression();
      // A pointer to a container is a tracked alias. void* and pointers
      // to scalars stay raw, in the container's C layout.
      if (t.depth == 1 && recordValue(t.pointee())) t.role = Type::Alias;
    }

    const bool lambdaDefault = initializer && initializer->kind == ExprKind::Lambda;

    if (funcValue(t) && !t.signature) {
      if (!lambdaDefault)
        fail(name, "func field '" + name.text + "' needs a signature or a lambda default, as in func int() " + name.text);

      t = initializer->castType;
    }

    record.fields.push_back({name, t, false, isProtected, false, std::move(initializer)});
    record.fields.back().usings = usings;
    record.fields.back().tags = std::move(tags);
    record.fields.back().memberLambda = funcValue(t) && lambdaDefault;

    if (scoped)
      markScoped(record.fields.back(), *scoped, record.fields.back().type, record.fields.back().initializer != nullptr);
  }
  // One instance body. Members accumulate across the chain's instance bodies.
  void endMember() {
    if (!at(Tok::RightBrace) && !at(Tok::Newline)) fail(peek(), "expected member terminator");

    lines();
  }
  // `bare` is the container's own name; the record carries its qualified one.
  void containerBody(Program &program, Record &record, std::set<std::string> &members,
                     const std::string &bare) {
    const auto &tag = record.name;
    const auto enclosing = scope;
    scope = tag.text;
    expect(Tok::LeftBrace);
    lines();

    while (!accept(Tok::RightBrace)) {
      if (macroDeclaration()) {
        skipMacroDeclaration();
        continue;
      }

      if (at(Tok::This) && punctuation(tokens.at(pos + 1), Tok::LeftBrace)) {
        markupBlock(program, record, members, bare);
        endMember();
        continue;
      }

      std::optional<Token> scoped;

      if (scopeMarkAhead()) scoped = scopeMark();

      const auto tagged = peek();
      auto tags = attributes();
      const auto modifier = peek();
      const bool isProtected = accessModifier();

      // ~Name() declares the container's destructor.
      if (at(Tok::Tilde)) {
        destructorDeclaration(program, members, bare, tag, tags, tagged, isProtected, modifier);
        endMember();
        continue;
      }

      bool typed = declarationType();
      Type t = typed ? type() : Type(Type::Void);
      auto name = identifier();

      // Name(...) declares a constructor, keyed by its parameter types;
      // -Name(...) declares a partial constructor.
      if (name.text == bare && at(Tok::LeftParen)) {
        constructorDeclaration(program, members, tag, name, typed, tags, tagged, isProtected);
        endMember();
        continue;
      }

      if (!members.insert(name.text).second) fail(name, "duplicate container member");
      if (name.text == Tok::This) fail(name, "this is reserved for the receiver");

      if (at(Tok::LeftParen) && !(typed && owningFieldAhead(t))) {
        if (scoped) fail(*scoped, "a scope mark applies to a field or a loop, not a method");

        name.text = methodKey(tag.text, name.text);
        program.push_back(function(std::move(name), t, false, "", tag.text));
        program.back().protectedAccess = isProtected;
        program.back().tags = std::move(tags);
      } else {
        containerField(record, name, t, typed, isProtected, std::move(tags), scoped);
      }

      endMember();
    }

    scope = enclosing;
  }
  // `T{`, written without a space, attaches tests to the container before it.
  // `T {` stays an ordinary container named T.
  bool testBody() const {
    if (!at(Tok::T)) return false;

    const auto &next = tokens.at(pos + 1);
    return !next.string && next.text == Tok::LeftBrace && next.source == peek().source && next.line == peek().line &&
           next.column == peek().column + 1;
  }
  // Each member is a test: `Name() { ... }`. Tests are static members of their
  // container, so they see what its other members see, protected ones too.
  void testBlock(Program &program, const std::string &container) {
    pop();
    const auto enclosing = scope;
    scope = container;
    expect(Tok::LeftBrace);
    lines();

    while (!accept(Tok::RightBrace)) {
      if (macroDeclaration()) {
        skipMacroDeclaration();
        continue;
      }

      const auto start = peek();

      if (declarationType())
        fail(start, "a T{ } block holds only tests, written Name() { ... } with no result type");

      auto name = identifier();

      if (!at(Tok::LeftParen)) fail(name, "a T{ } block holds only tests, written Name() { ... }");

      auto key = name;
      key.text = container + ".$test$" + name.text;
      currentTest = container + "." + name.text;
      auto test = function(std::move(key), Type::Void, false, "");
      test.testName = std::exchange(currentTest, "");

      if (!test.params.empty()) fail(name, "a test takes no parameters");

      program.tests.push_back(std::move(test));

      if (!at(Tok::RightBrace) && !at(Tok::Newline)) fail(peek(), "expected member terminator");

      lines();
    }

    scope = enclosing;
  }
  // `#{` starts a static body.
  bool staticBody() const {
    return at(Tok::Hash) && !tokens.at(pos + 1).string && tokens.at(pos + 1).text == Tok::LeftBrace;
  }
  // `@{` starts a getter block and `={` a setter block.
  bool accessorBody() const {
    return (at(Tok::At) || at(Tok::Assign)) && !tokens.at(pos + 1).string && tokens.at(pos + 1).text == Tok::LeftBrace;
  }
  // A getter block, `@{ Age: Years }`, or a setter block, `={ Age: Years =
  // value }`. Each entry declares one accessor of a property: an optional
  // type, the name, ':', and a getter expression or a setter statement, or a
  // `{ ... }` body. A setter reads the assigned value as `value`. Accessors
  // are instance methods named get-Name and set-Name.
  void accessorBlock(Program &program, const std::string &record, std::set<std::string> &members) {
    const bool getter = pop().text == Tok::At;
    const auto enclosing = scope;
    scope = record;
    expect(Tok::LeftBrace);
    lines();

    while (!accept(Tok::RightBrace)) {
      const bool isProtected = accessModifier();
      const bool typed = declarationType();
      const Type t = typed ? type() : Type(Type::Void);

      if (typed && t == Type::Void) fail(peek(), "a property requires a value type");

      auto name = identifier();

      if (name.text == Tok::This) fail(name, "this is reserved for the receiver");

      expect(Tok::Colon);

      // A `{ ... }` body may start on the next line.
      if (const auto saved = pos; (lines(), !at(Tok::LeftBrace))) pos = saved;

      Function f{name, getter ? t : Type(Type::Void), {}, {}, true, {}};
      f.name.text = getter ? getterKey(record, name.text) : setterKey(record, name.text);

      if (!members.insert(f.name.text).second)
        fail(name, "property '" + name.text + "' already has a " + (getter ? "getter" : "setter"));

      f.usings = usings;
      f.protectedAccess = isProtected;
      f.inferred = !typed;
      Type self = Type::Record;
      self.record = record;
      Token receiver = name;
      receiver.text = Tok::This;
      f.params.push_back({receiver, self, true});

      if (getter) {
        if (at(Tok::LeftBrace)) {
          if (!typed)
            fail(peek(), "a getter with a block body needs a type, as in int " + name.text + ": { ... }");

          f.body = block();
        } else {
          auto result = std::make_unique<Stmt>();
          result->kind = StmtKind::Return;
          result->token = peek();
          result->expr = expression();
          f.body.push_back(std::move(result));
        }
      } else {
        Token value = name;
        value.text = Tok::ImplicitValue;
        f.params.push_back({value, t, false});

        if (at(Tok::LeftBrace)) f.body = block();
        else f.body.push_back(statement());
      }

      program.push_back(std::move(f));

      if (!accept(Tok::Comma) && !at(Tok::RightBrace) && !at(Tok::Newline)) fail(peek(), "expected newline, ',' or '}' after a property accessor");

      lines();
    }

    scope = enclosing;
  }
  // A container name followed by adjacent bodies in any order: `{ ... }`
  // holds instance members and `#{ ... }` static ones. Bodies are separated
  // by at most one newline; a blank line ends the chain and is rejected.
  void containerChain(Program &program, const Token &tag, std::vector<Token> parents,
                      const std::string &prefix, bool nested) {
    // A container nested in a static body is named by its enclosing path.
    Token qualified = tag;
    qualified.text = prefix + tag.text;
    Record record{qualified, {}, std::move(parents)};
    std::set<std::string> members;
    bool instance = false;

    for (bool first = true;; first = false) {
      if (!first) {
        auto saved = pos;
        accept(Tok::Newline);

        if (at(Tok::Newline)) {
          lines();

          if (at(Tok::LeftBrace) || staticBody() || accessorBody() || testBody())
            fail(peek(), "chained container bodies must be adjacent, without a blank line");
        }

        if (!at(Tok::LeftBrace) && !staticBody() && !accessorBody() && !testBody()) {
          pos = saved;
          break;
        }
      }

      if (testBody()) {
        testBlock(program, qualified.text);
        continue;
      }

      if (accept(Tok::Hash)) {
        declarations(program, prefix + tag.text + ".", true, true);
        continue;
      }

      if (accessorBody()) {
        accessorBlock(program, qualified.text, members);
        continue;
      }

      containerBody(program, record, members, tag.text);
      instance = true;
    }

    if (instance) program.records.push_back(std::move(record));
    else if (!record.parents.empty())
      fail(record.parents.front(), "a parent list requires an instance body");

    if (!at(Tok::Eof) && !at(Tok::Newline) && !(nested && at(Tok::RightBrace)))
      fail(peek(), "expected newline after container");

    lines();
  }

  // After a file-scope name, ':' starts either an enum (`Name : type #@{`)
  // or a parent list (`Name : Parent, Other {`). Whichever of '#' or '{'
  // comes first decides.
  bool parentList() const {
    if (!at(Tok::Colon)) return false;

    for (size_t i = pos + 1; i < tokens.size(); ++i) {
      const auto &t = tokens[i];

      if (t.string) continue;
      if (t.text == Tok::LeftBrace) return true;

      // An enum continues with `#@{`; a parent list may start with a static body.
      if (t.text == Tok::Hash)
        return i + 1 < tokens.size() && !tokens[i + 1].string && tokens[i + 1].text == Tok::LeftBrace;

      if (t.text == Tok::Eof) return false;
    }

    return false;
  }
  bool enumBlock() const {
    return at(Tok::Hash) && !tokens.at(pos + 1).string && tokens.at(pos + 1).text == Tok::At;
  }
  // Name [: type] #@{ Member[: literal], ... } with members separated by
  // commas or newlines. The type may be omitted on an extension fragment.
  void enumFragment(Program &program, Token tag) {
    EnumFragment fragment{tag, Type::Void, {}};

    if (accept(Tok::Colon)) {
      const auto underlying = peek();
      fragment.type = type();
      if (fragment.type == Type::Void) fail(underlying, "enum underlying type cannot be void");
      lines();
    }

    if (!enumBlock()) fail(peek(), "expected '#@{' after enum declaration");

    pop();
    pop();
    lines();
    expect(Tok::LeftBrace);
    lines();

    while (!accept(Tok::RightBrace)) {
      EnumMember member{identifier(), nullptr};

      if (accept(Tok::Colon)) member.value = expression();

      fragment.members.push_back(std::move(member));

      if (!at(Tok::RightBrace) && !at(Tok::Newline) && !at(Tok::Comma)) fail(peek(), "expected ',' or newline between enum members");

      accept(Tok::Comma);
      lines();
    }

    program.enums.push_back(std::move(fragment));
  }

  // A dynamic array is move-only, and the native entry owns the argument
  // array, so `Main(string [] args)` borrows it instead of taking a copy.
  static void entryParameter(Function &main) {
    if (main.params.size() == 1 && dynamicArray(main.params[0].type))
      main.params[0].reference = true;
  }

  // A function definition ahead: `[type] Name(...)` and then its block. The
  // block is the only difference between a definition and a call.
  bool functionAhead() {
    auto punct = [&](size_t k, auto text) { return !tokens[k].string && tokens[k].text == text; };
    size_t k = pos;

    // A func result's own signature is not the parameter list.
    if (at(Tok::Func)) {
      const auto saved = pos;
      ++pos;
      const bool signature = funcSignatureAhead();
      pos = saved;

      if (signature) {
        while (!punct(k, Tok::LeftParen) && !punct(k, Tok::Eof) && !punct(k, Tok::Newline)) ++k;

        for (int depth = 0; !punct(k, Tok::Eof); ++k) {
          if (punct(k, Tok::LeftParen)) ++depth;
          else if (punct(k, Tok::RightParen) && --depth == 0) {
            ++k;
            break;
          }
        }
      }
    }

    for (; !punct(k, Tok::LeftParen); ++k)
      if (tokens[k].string || punct(k, Tok::Eof) || punct(k, Tok::Newline) || punct(k, Tok::Assign) || punct(k, Tok::Colon) || punct(k, Tok::ColonColon))
        return false;

    if (!skipToCloseParen(k)) return false;

    for (++k; punct(k, Tok::Newline); ++k) {
    }

    return punct(k, Tok::LeftBrace);
  }
  bool containerAhead() {
    // A static-only container is not a type name, but still has a body.
    const bool named = namedType();
    const auto saved = pos;
    ++pos;
    lines();
    const bool found = at(Tok::LeftBrace) || staticBody() || (named && (parentList() || at(Tok::Colon) || enumBlock()));
    pos = saved;
    return found;
  }
  // Anything at file scope that is not a declaration is a statement. Tokens
  // that open no statement stay on the declaration path and its diagnostics.
  bool fileStatement() {
    if (at(Tok::Extern) || at(Tok::LeftBrace) || at(Tok::RightBrace) || at(Tok::Plus) || at(Tok::Minus)) return false;

    if (at(Tok::Hash)) {
      const auto &next = tokens.at(pos + 1);
      return next.string || (next.text != Tok::LeftBrace && next.text != Tok::At);
    }

    if (at(Tok::If) || at(Tok::For) || at(Tok::While) || at(Tok::Do) || at(Tok::Return) || at(Tok::Break) || at(Tok::Continue) ||
        at(Tok::Switch))
      return true;

    return !functionAhead() && !containerAhead();
  }
  bool macroDeclaration() const {
    return at(Tok::Macro) && word(tokens.at(pos + 1)) && punctuation(tokens.at(pos + 2), Tok::LeftParen);
  }
  // collectMacros already captured it; a macro's body is checked only when used.
  void skipMacroDeclaration() {
    int depth = 0;
    pos += 2;

    for (;; ++pos) {
      if (at(Tok::LeftParen)) ++depth;
      else if (at(Tok::RightParen) && --depth == 0) break;
    }

    ++pos;
    lines();
    depth = 0;

    for (;; ++pos) {
      if (at(Tok::LeftBrace)) ++depth;
      else if (at(Tok::RightBrace) && --depth == 0) break;
    }

    ++pos;

    if (!at(Tok::Newline) && !at(Tok::RightBrace) && !at(Tok::Eof)) fail(peek(), "expected newline after macro");

    lines();
  }
  // Collects a file's statements into its hidden function, named by
  // combineUnits. Its locals live for one Exec; other files never see them.
  void fileScript(Program &program, std::vector<std::unique_ptr<Stmt>> body) {
    Token name = body.front()->token;
    name.text = Tok::FirstScript;
    name.string = name.character = false;
    Function script{std::move(name), Type::Void, {}, std::move(body), false, {}};
    script.script = true;
    script.usings = usings;
    program.push_back(std::move(script));
  }

  bool attributeEnumMark(bool nested) {
    const auto opening = peek();
    const auto tags = attributes();

    if (tags.size() != 1 || tags[0].token.text != Tok::Args || !namedType())
      fail(opening, nested ? "attributes on static members are not supported yet; only [args] can mark an enum here"
                           : "only [args] can precede a file-scope declaration, and it marks an enum");

    return true;
  }
  bool namedDeclaration(Program &functions, const std::string &prefix, bool nested, const Token &modifier, bool modified, bool attributeEnum) {
    auto saved = pos;
    auto tag = identifier();
    lines();

    if (parentList() || at(Tok::LeftBrace) || staticBody()) {
      if (attributeEnum) fail(tag, "[args] marks an enum, not a container");
      if (modified) fail(modifier, "access modifiers on nested containers are not supported");

      std::vector<Token> parents;

      if (accept(Tok::Colon)) {
        do {
          lines();
          parents.push_back(parentName());
          lines();
        } while (accept(Tok::Comma));
      }

      containerChain(functions, tag, std::move(parents), prefix, nested);
      return true;
    }

    if (attributeEnum && !at(Tok::Colon) && !enumBlock()) fail(tag, "[args] marks an enum, not a container");

    if (at(Tok::Colon) || enumBlock()) {
      // An enum in a static body is named by its enclosing containers, as
      // in Outer.Day; inside Outer it is simply Day.
      tag.text = prefix + tag.text;
      enumFragment(functions, tag);
      functions.enums.back().attribute = attributeEnum;

      if (!at(Tok::Eof) && !at(Tok::Newline) && !(nested && at(Tok::RightBrace))) fail(peek(), "expected newline after enum");

      lines();
      return true;
    }

    pos = saved;
    return false;
  }
  void staticField(Program &functions, Token name, const Type &result, const std::string &prefix, bool constant, bool isProtected, const std::optional<Token> &scoped) {
    if (result == Type::Void) fail(name, "static field cannot have void type");

    name.text = prefix + name.text;
    StaticField field{name, result, constant, isProtected, nullptr};
    field.usings = usings;

    if (accept(Tok::Assign))
      field.initializer = expression();
    else if (constant)
      fail(peek(), "constant '" + name.text + "' requires an initializer, as in #int Max = 10");

    if (scoped) {
      if (constant) fail(*scoped, "a constant cannot live in a scope");

      markScoped(field, *scoped, field.type, field.initializer != nullptr);
    }

    functions.statics.push_back(std::move(field));

    if (!at(Tok::Newline) && !at(Tok::RightBrace)) fail(peek(), "expected newline after static field");

    lines();
  }
  void fileStatement(std::vector<std::unique_ptr<Stmt>> &statements) {
    statements.push_back(statement());

    if (!at(Tok::Newline)) fail(peek(), "expected newline between statements");

    lines();
  }
  // With braced, reads one `{ ... }` static body; otherwise reads to <eof>.
  void declarations(Program &functions, const std::string &prefix,
                    bool nested, bool braced = false) {
    const auto enclosing = scope;
    const auto outer = usings;
    scope = prefix.empty() ? "" : prefix.substr(0, prefix.size() - 1);

    if (braced) expect(Tok::LeftBrace);

    lines();
    usingDirectives();
    std::vector<std::unique_ptr<Stmt>> statements;

    while (!at(Tok::Eof) && !(nested && at(Tok::RightBrace))) {
      if (testBody())
        fail(peek(), "a T{ } test block must directly follow a container's body, with no blank line between them");

      if (at(Tok::Using))
        fail(peek(), "using directives must come before the other declarations in their scope");

      if (macroDeclaration()) {
        skipMacroDeclaration();
        continue;
      }

      if (nested && macroUse(tokens, pos) && !functionAhead())
        fail(peek(), "a macro expands in statement, Switch, or value position, not among declarations");

      // A marked loop at file scope is a statement like any other loop.
      if (!nested && scopeMarkAhead()) {
        fileStatement(statements);
        continue;
      }

      std::optional<Token> scoped;

      if (scopeMarkAhead()) scoped = scopeMark();

      // `[args]` marks the enum that follows as attribute data.
      const bool attributeEnum = at(Tok::LeftBracket) && attributeEnumMark(nested);

      if (!nested && !attributeEnum && fileStatement()) {
        fileStatement(statements);
        continue;
      }

      const auto modifier = peek();
      const bool modified = at(Tok::Plus) || at(Tok::Minus);

      if (modified && !nested) fail(modifier, "access modifiers apply only to container members");

      const bool isProtected = accessModifier();
      bool external = accept(Tok::Extern);

      if (external) {
        if (!peek().string || peek().character || peek().text != Tok::C)
          fail(peek(), "native declarations require extern \"C\"");

        pop();
      }

      if (!external && namedType() && namedDeclaration(functions, prefix, nested, modifier, modified, attributeEnum)) continue;

      // In a static block, `#int Max = 10` declares a constant static field.
      const auto constantToken = peek();
      const bool constant = nested && !external && accept(Tok::Hash);

      if (constant && !declarationType())
        fail(peek(), "constant static field requires an explicit type");

      bool typed = declarationType();
      Type result = typed ? type() : Type::Void;

      if (at(Tok::RightBrace) && !nested) fail(peek(), "unmatched '}': no block is open here");

      Token name = identifier();

      // `itn F() { ... }`: the word before the function name is not a type.
      if (!typed && !external && isWord(peek()) && !tokens.at(pos + 1).string && tokens.at(pos + 1).text == Tok::LeftParen)
        unknownType(name);

      if (nested && !external && typed && !at(Tok::LeftParen)) {
        staticField(functions, name, result, prefix, constant, isProtected, scoped);
        continue;
      }

      if (scoped) fail(*scoped, "a scope mark applies to a field or a loop");
      if (constant) fail(constantToken, "'#' before a member declares a constant static field");

      if (!external && !typed && (at(Tok::Colon) || enumBlock()))
        fail(name, "enum declarations must be at file scope");

      if (!external && !typed && !at(Tok::LeftParen)) {
        lines();

        if (staticBody() || at(Tok::LeftBrace)) {
          if (modified)
            fail(modifier, "access modifiers on nested containers are not supported");

          containerChain(functions, name, {}, prefix, nested);
          continue;
        }
      }

      name.text = prefix + name.text;
      auto nativeName = name.text.substr(prefix.size());
      functions.push_back(function(std::move(name), result, external, nativeName));
      functions.back().protectedAccess = isProtected;

      if (!nested && functions.back().name.text == Tok::Main) entryParameter(functions.back());

      if (!at(Tok::Eof) && !at(Tok::Newline) && !(nested && at(Tok::RightBrace)))
        fail(peek(), "expected newline between functions");

      lines();
    }

    if (braced) expect(Tok::RightBrace);
    if (!statements.empty()) fileScript(functions, std::move(statements));

    scope = enclosing;
    usings = outer;
  }
  // `using Name.Space` lines at the top of a file or static body. The target
  // resolves like a type path, from the innermost container outward.
  void usingDirectives() {
    if (!at(Tok::Using)) return;

    auto visible = *usings;

    while (at(Tok::Using)) {
      const auto keyword = pop();
      auto start = peek();
      std::string path = identifier().text;

      while (accept(Tok::Dot)) path += "." + identifier().text;

      start.text = path;
      std::string target;

      for (auto enclosing = scope;;) {
        const auto candidate = enclosing.empty() ? path : enclosing + "." + path;

        if (containerNames.count("#{" + candidate) || containerNames.count(candidate)) {
          target = candidate;
          break;
        }

        if (enclosing.empty()) break;

        const auto dot = enclosing.rfind('.');
        enclosing = dot == std::string::npos ? "" : enclosing.substr(0, dot);
      }

      if (target.empty()) fail(start, "unknown container '" + path + "' in using directive");

      for (const auto &directive : visible)
        if (directive.level == scope && directive.target == target)
          fail(start, "duplicate using directive for '" + target + "'");

      visible.push_back({start, scope, target});
      (void)keyword;

      if (!at(Tok::Newline) && !at(Tok::Eof) && !at(Tok::RightBrace)) fail(peek(), "expected newline after using directive");

      lines();
    }

    usings = std::make_shared<const std::vector<Using>>(std::move(visible));
  }
  // A parent in a parent list, resolved like any type path. An unknown name
  // is kept as written for the inheritance check to report.
  Token parentName() {
    std::string qualified;
    const auto length = typePath(pos, &qualified);
    auto token = identifier();

    if (length) {
      for (size_t k = 1; k < length; ++k) pop();

      token.text = qualified;
    }

    return token;
  }

public:
  Parser(std::vector<Token> t, const std::set<std::string> &names, const MacroTable &macroTable)
      : tokens(std::move(t)), containerNames(names), macros(macroTable), taskContainer(names.count("task") != 0),
        fileContainer(names.count("file") != 0), markupContainer(names.count("markup") != 0),
        pContainer(names.count("P") != 0), tContainer(names.count("T") != 0 || names.count("#{T") != 0) {}
  Program parse() {
    Program functions;
    output = &functions;
    functions.sources.push_back(sourcePath(tokens.front().source));
    // The last statement of a file need not end in a newline.
    tokens.insert(tokens.end() - 1, Token{Tok::Newline, tokens.back().line, tokens.back().column, false, tokens.back().source});
    declarations(functions, "", false);
    return functions;
  }
  // The tokens as they were given, after parse(): every expansion is undone,
  // latest first, then the closing newline is removed.
  std::vector<Token> restoredTokens() && {
    for (auto s = splices.rbegin(); s != splices.rend(); ++s) splice(s->first, s->count, std::move(s->replaced));

    tokens.erase(tokens.end() - 2);
    return std::move(tokens);
  }
};
} // namespace
Program parse(std::vector<Token> tokens) {
  tokens = preprocess(std::move(tokens));
  std::set<std::string> names;
  MacroTable macros;
  collectContainerNames(tokens, names);
  collectMacros(tokens, macros);
  return parse(std::move(tokens), names, macros);
}
namespace {
bool punctuation(const std::vector<Token> &tokens, size_t k, Tok kind) {
  return k < tokens.size() && !tokens[k].string && tokens[k].text == kind;
}
// The '}' matching the '{' at open, or the end of the tokens.
size_t closingBrace(const std::vector<Token> &tokens, size_t open) {
  int nesting = 0;

  for (size_t k = open; k < tokens.size(); ++k) {
    if (tokens[k].string) continue;

    if (tokens[k].text == Tok::LeftBrace) ++nesting;
    else if (tokens[k].text == Tok::RightBrace && --nesting == 0) return k;
  }

  return tokens.size();
}
// Whether [k, end) starts with comma-separated dotted names leading to a
// body: a parent list, or an enum's underlying type.
bool namesBeforeBody(const std::vector<Token> &tokens, size_t k, size_t end) {
  auto skipLines = [&] { while (k < end && punctuation(tokens, k, Tok::Newline)) ++k; };
  auto word = [&] {
    return k < end && isWord(tokens[k]);
  };

  while (true) {
    skipLines();

    if (!word()) return false;

    for (++k; punctuation(tokens, k, Tok::Dot) ; ++k) {
      ++k;

      if (!word()) return false;
    }

    skipLines();

    if (k < end && (punctuation(tokens, k, Tok::LeftBrace) || punctuation(tokens, k, Tok::Hash))) return true;
    if (!punctuation(tokens, k, Tok::Comma)) return false;

    ++k;
  }
}
// Reads `macro Name(type name, ...) { body }` at i, into the table when one
// is given. Returns the index of the closing brace.
size_t collectMacro(const std::vector<Token> &tokens, size_t i, const std::string &prefix, MacroTable *macros) {
  auto isWord = [&](size_t k) {
    return k < tokens.size() && duck::isWord(tokens[k]);
  };
  Macro macro;
  macro.name = tokens[i + 1];
  size_t k = i + 3;
  std::set<std::string> seen;

  for (;;) {
    while (punctuation(tokens, k, Tok::Newline)) ++k;
    if (punctuation(tokens, k, Tok::RightParen)) break;

    MacroParameter parameter;
    int depth = 0;

    for (;; ++k) {
      if (k >= tokens.size() || punctuation(tokens, k, Tok::Eof)) fail(macro.name, "expected ')' after macro parameters");
      if (depth == 0 && (punctuation(tokens, k, Tok::Comma) || punctuation(tokens, k, Tok::RightParen))) break;

      if (punctuation(tokens, k, Tok::LeftParen) || punctuation(tokens, k, Tok::LeftBracket)) ++depth;
      else if (punctuation(tokens, k, Tok::RightParen) || punctuation(tokens, k, Tok::RightBracket)) --depth;

      if (!punctuation(tokens, k, Tok::Newline)) parameter.type.push_back(tokens[k]);
    }

    if (parameter.type.empty()) fail(tokens[k], "expected a macro parameter");

    parameter.name = parameter.type.back();
    parameter.type.pop_back();

    if (!isWord(k - 1) || parameter.type.empty())
      fail(parameter.name, "a macro parameter requires an explicit type and a name, as in int count");

    if (!parameter.type.back().string && parameter.type.back().text == Tok::Ampersand)
      fail(parameter.name, "macro parameter '" + parameter.name.text + "' cannot be a reference: arguments are bound "
                           "once by value; a macro body can name the caller's variable directly");

    if (!seen.insert(parameter.name.text).second)
      fail(parameter.name, "duplicate macro parameter '" + parameter.name.text + "'");

    macro.parameters.push_back(std::move(parameter));

    if (punctuation(tokens, k, Tok::Comma)) ++k;
  }

  ++k;

  while (punctuation(tokens, k, Tok::Newline)) ++k;
  if (!punctuation(tokens, k, Tok::LeftBrace)) fail(macro.name, "macro '" + macro.name.text + "' requires a { ... } body");

  const size_t close = closingBrace(tokens, k);

  if (close >= tokens.size()) fail(tokens[k], "unterminated macro body");

  macro.body.assign(tokens.begin() + long(k) + 1, tokens.begin() + long(close));

  if (macros) {
    const auto name = macro.name;
    const auto inserted = macros->emplace(prefix + name.text, std::move(macro));

    if (!inserted.second)
      fail(name, "macro '" + prefix + name.text + "' is already defined at " + where(inserted.first->second.name));
  }

  return close;
}
// Collects the type names declared in [begin, end), one declaration level,
// qualified by prefix. Static bodies are scanned with the container's name
// appended to the prefix, so `Name #{ Space { } }` registers Name.Space.
void collectScope(const std::vector<Token> &tokens, size_t begin, size_t end,
                  const std::string &prefix, std::set<std::string> &names, MacroTable *macros = nullptr) {
  for (size_t i = begin; i < end; ++i) {
    const auto &t = tokens[i];

    if (t.string) continue;

    // Function bodies and other blocks declare no types.
    if (t.text == Tok::LeftBrace) {
      i = closingBrace(tokens, i);
      continue;
    }

    if (!isIdentifierStart(t.text[0])) continue;

    if (t.text == Tok::Macro && i + 2 < end && !tokens[i + 1].string && !tokens[i + 1].text.empty() &&
        (std::isalpha(static_cast<unsigned char>(tokens[i + 1].text[0])) || tokens[i + 1].text[0] == '_') &&
        punctuation(tokens, i + 2, Tok::LeftParen)) {
      i = collectMacro(tokens, i, prefix, macros);
      continue;
    }

    // A file-scope statement declares no types, but its header can look as
    // if it does: `if ready {` and `for x : items {`. Skip the whole line.
    if (t.text == Tok::If || t.text == Tok::Else || t.text == Tok::For || t.text == Tok::While || t.text == Tok::Do) {
      while (i < end && !punctuation(tokens, i, Tok::Newline)) {
        if (punctuation(tokens, i, Tok::LeftBrace)) i = closingBrace(tokens, i);

        ++i;
      }

      continue;
    }

    size_t next = i + 1;

    while (punctuation(tokens, next, Tok::Newline)) ++next;
    if (next >= end) continue;
    // `x : 1` is a statement; a declaration's ':' starts a list of names.
    if (punctuation(tokens, next, Tok::Colon) && !namesBeforeBody(tokens, next + 1, end)) continue;

    size_t chain = next;

    // ':' after a name starts an enum's underlying type or a container's
    // parent list. A parent list reaches '{'
    // or '#{' before any other '#'. Skip the list or type so its words are
    // never taken as declared names.
    if (punctuation(tokens, next, Tok::Colon) ||
        (punctuation(tokens, next, Tok::Hash) && punctuation(tokens, next + 1, Tok::At))) {
      size_t stop = next;

      while (stop < end && (tokens[stop].string || (tokens[stop].text != Tok::Hash && tokens[stop].text != Tok::LeftBrace)))
        ++stop;

      const bool parents = punctuation(tokens, next, Tok::Colon) && stop < end &&
                           (punctuation(tokens, stop, Tok::LeftBrace) ||
                            (punctuation(tokens, stop, Tok::Hash) && punctuation(tokens, stop + 1, Tok::LeftBrace)));

      if (!parents) {
        names.insert(prefix + t.text);
        names.insert("#@" + prefix + t.text);
        i = stop - 1;
        continue;
      }

      chain = stop;
    }

    // A chain of `{...}` and `#{...}` bodies; any instance body names a type.
    bool instance = false;
    size_t last = std::string::npos;

    for (size_t k = chain;;) {
      size_t open;

      // A chained T{ } block declares no type; only macros matter inside it.
      if (last != std::string::npos && punctuation(tokens, k, Tok::T) && punctuation(tokens, k + 1, Tok::LeftBrace) &&
          tokens[k + 1].line == tokens[k].line && tokens[k + 1].column == tokens[k].column + 1) {
        const size_t close = closingBrace(tokens, k + 1);

        if (macros) collectScope(tokens, k + 2, close, prefix + t.text + ".", names, macros);

        last = close;
        k = close + 1;

        if (punctuation(tokens, k, Tok::Newline)) ++k;

        continue;
      }

      if (punctuation(tokens, k, Tok::LeftBrace)) open = k;
      else if (punctuation(tokens, k, Tok::Hash) && punctuation(tokens, k + 1, Tok::LeftBrace)) open = k + 1;
      else break;

      const size_t close = closingBrace(tokens, open);

      if (open == k) {
        instance = true;

        // Instance bodies declare no types here, but they may hold macros.
        if (macros) collectScope(tokens, open + 1, close, prefix + t.text + ".", names, macros);
      } else {
        names.insert("#{" + prefix + t.text);
        collectScope(tokens, open + 1, close, prefix + t.text + ".", names, macros);
      }

      last = close;
      k = close + 1;

      if (punctuation(tokens, k, Tok::Newline)) ++k;
    }

    if (last == std::string::npos) continue;
    // A parent list also names a type, so the parser can report a chain
    // that lacks the instance body the list requires.
    if (instance || chain != next) names.insert(prefix + t.text);

    i = last;
  }
}
} // namespace
void collectContainerNames(const std::vector<Token> &tokens, std::set<std::string> &names) {
  collectScope(tokens, 0, tokens.size(), "", names);
}
void collectMacros(const std::vector<Token> &tokens, MacroTable &macros) {
  std::set<std::string> names;
  collectScope(tokens, 0, tokens.size(), "", names, &macros);
}
Program parse(std::vector<Token> tokens, const std::set<std::string> &names) {
  return parse(std::move(tokens), names, MacroTable{});
}
Program parse(std::vector<Token> tokens, const std::set<std::string> &names, const MacroTable &macros) {
  return Parser(std::move(tokens), names, macros).parse();
}
Program parseKeeping(std::vector<Token> &tokens, const std::set<std::string> &names, const MacroTable &macros) {
  Parser parser(std::move(tokens), names, macros);
  auto program = parser.parse();
  tokens = std::move(parser).restoredTokens();
  return program;
}
} // namespace duck
