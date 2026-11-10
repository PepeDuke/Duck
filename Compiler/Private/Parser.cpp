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
  const std::set<std::string> &containerNames;
  const MacroTable &macros;
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
  bool at(const std::string &s) const {
    return !peek().string && peek().text == s;
  }
  Token pop() { return tokens[pos++]; }
  bool accept(const std::string &s) {
    if (!at(s))
      return false;

    pop();
    return true;
  }
  static std::string describe(const Token &t) {
    if (t.string) return t.character ? "a character literal" : "a string literal";
    if (t.text == "<eof>") return "end of file";
    if (t.text == "\n") return "end of line";

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
  Token expect(const std::string &s) {
    if (!at(s)) expected(s);

    return pop();
  }
  [[noreturn]] void badStatementEnd(const std::string &suffix = "") const {
    if (at(";")) fail(peek(), "expected newline after statement, found ';'; Duck statements end at a newline, so remove it");

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
    while (accept("\n")) {
    }
  }
  Token identifier() {
    auto t = peek();

    if (!isWord(t))
      fail(t, "expected identifier, found " + describe(t));

    if (isType() || t.text == "true" || t.text == "false" ||
        t.text == "return" || t.text == "if" || t.text == "else" ||
        t.text == "while" || t.text == "for" || t.text == "break" ||
        t.text == "continue" || t.text == "do" || t.text == "extern" ||
        t.text == "null" || t.text == "using" || t.text == "Switch")
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
  static bool punctuation(const Token &t, const char *text) { return !t.string && t.text == text; }
  static bool openingBracket(const Token &t) { return punctuation(t, "(") || punctuation(t, "[") || punctuation(t, "{"); }
  static bool closingBracket(const Token &t) { return punctuation(t, ")") || punctuation(t, "]") || punctuation(t, "}"); }
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

    while (k + 1 < list.size() && punctuation(list[k], ".") && word(list[k + 1])) {
      path += "." + list[k + 1].text;
      k += 2;
    }

    if (k >= list.size() || !punctuation(list[k], "(")) return std::nullopt;

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
      if (k >= tokens.size() || punctuation(tokens[k], "<eof>")) fail(tokens[use.open], "expected ')'");

      const auto &t = tokens[k];

      if (depth == 1 && punctuation(t, "\n")) {
        lineBreak = !arguments.back().empty();
        continue;
      }

      if (openingBracket(t)) ++depth;
      else if (closingBracket(t)) {
        if (--depth == 0) {
          close = k;
          break;
        }
      } else if (depth == 1 && punctuation(t, ",")) {
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
  void replaceTokens(size_t first, size_t last, std::vector<Token> replacement) {
    tokens.erase(tokens.begin() + long(first), tokens.begin() + long(last) + 1);
    tokens.insert(tokens.begin() + long(first), std::make_move_iterator(replacement.begin()),
                  std::make_move_iterator(replacement.end()));
  }
  // In statement position a macro becomes a block: its parameters and locals
  // end with it, while break, continue, and return act on the caller.
  void expandStatementMacro() {
    auto use = macroUse(tokens, pos);

    if (!use) return;

    size_t close = 0;
    auto arguments = macroArguments(*use, close);

    if (close + 1 >= tokens.size() ||
        !(punctuation(tokens[close + 1], "\n") || punctuation(tokens[close + 1], "}") ||
          punctuation(tokens[close + 1], "<eof>")))
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
      expect("\n");
    }
  }
  // Whether a macro's output starts with a Switch label, directly or through
  // the macro it calls first.
  bool opensSection(const MacroUse &use, unsigned depth = 0) const {
    const auto &body = use.macro->body;
    size_t k = 0;

    while (k < body.size() && punctuation(body[k], "\n")) ++k;
    if (k == body.size()) return false;
    if (punctuation(body[k], "case")) return true;
    if (punctuation(body[k], "default") && k + 1 < body.size() && punctuation(body[k + 1], ":")) return true;
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

    while (first < last && punctuation(macro.body[first], "\n")) ++first;
    while (last > first && punctuation(macro.body[last - 1], "\n")) --last;
    if (first == last) fail(tokens[pos], "macro '" + use.qualified + "' is empty, so it cannot be used as a value");

    int depth = 0;

    for (size_t k = first; k < last; ++k) {
      const auto &t = macro.body[k];

      if (openingBracket(t)) ++depth;
      else if (closingBracket(t)) --depth;
      else if (depth == 0 && punctuation(t, "\n"))
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
    return at("case") || (at("default") && punctuation(tokens.at(pos + 1), ":"));
  }
  // `case a, b:` and `default:`, including labels stacked on one section.
  void sectionLabels(Stmt &switchStatement, Stmt &section) {
    for (;;) {
      if (accept("default")) {
        if (section.isDefault) fail(section.token, "a Switch section cannot repeat default");

        section.isDefault = true;
      } else {
        expect("case");

        do {
          lines();
          section.labels.push_back(expression());
          lines();
        } while (accept(","));
      }

      expect(":");
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
    s->kind = "switch";
    s->token = pop();
    expect("(");
    lines();
    auto selector = std::make_unique<Stmt>();
    selector->kind = "let";
    selector->token = peek();
    selector->name = "$switch";
    selector->expr = expression();
    lines();
    expect(")");
    s->target = std::make_unique<Expr>();
    s->target->kind = "name";
    s->target->token = selector->token;
    s->target->token.text = selector->name;
    s->target->token.string = s->target->token.character = false;
    s->initializer = std::move(selector);
    lines();
    expect("{");
    lines();
    hygiene.emplace_back();

    while (!at("}")) {
      if (at("<eof>")) expected("}");

      if (auto use = macroUse(tokens, pos)) {
        expandCaseMacro(*s, *use);
        lines();
        continue;
      }

      if (!atSectionLabel()) fail(peek(), "expected 'case' or 'default' in Switch body");

      auto section = std::make_unique<Stmt>();
      section->kind = "case";
      section->token = peek();
      sectionLabels(*s, *section);
      hygiene.emplace_back();

      while (!at("}") && !atSectionLabel()) {
        if (at("<eof>")) expected("}");
        if (auto use = macroUse(tokens, pos); use && opensSection(*use)) break;

        section->body.push_back(statement());

        if (!at("}") && !at("\n")) fail(peek(), "expected newline between statements");

        lines();
      }

      hygiene.pop_back();
      s->body.push_back(std::move(section));
    }

    hygiene.pop_back();
    expect("}");

    if (s->body.empty()) fail(s->token, "a Switch needs at least one case or default");

    return s;
  }
  // The tokens a container or enum type path at `at` spans, or 0. A path
  // such as `Space` or `Name.Space` resolves like a relative function name:
  // from the innermost enclosing container outward, then at file scope,
  // longest path first. `qualified` receives the type's full name.
  size_t typePath(size_t at, std::string *qualified = nullptr) const {
    std::vector<std::string> paths;

    for (size_t k = at;; k += 2) {
      const auto &t = tokens[k];

      if (!isWord(t))
        break;

      paths.push_back(paths.empty() ? t.text : paths.back() + "." + t.text);

      if (tokens[k + 1].string || tokens[k + 1].text != ".") break;
    }

    for (auto enclosing = scope; !paths.empty();) {
      for (size_t n = paths.size(); n-- > 0;) {
        const auto name = enclosing.empty() ? paths[n] : enclosing + "." + paths[n];

        if (containerNames.count(name)) {
          if (qualified) *qualified = name;

          return 2 * n + 1;
        }
      }

      // Then the containers this scope imports. Two imports providing the
      // path make it ambiguous.
      std::string found, via;
      size_t length = 0;

      for (const auto &directive : *usings) {
        if (directive.level != enclosing) continue;

        for (size_t n = paths.size(); n-- > 0;) {
          const auto name = directive.target + "." + paths[n];

          if (!containerNames.count(name)) continue;

          if (!found.empty() && found != name)
            fail(tokens[at], "'" + paths[n] + "' is ambiguous between '" + found + "' (using " +
                                 via + ") and '" + name + "' (using " + directive.target + ")");

          found = name;
          via = directive.target;
          length = 2 * n + 1;
          break;
        }
      }

      if (!found.empty()) {
        if (qualified) *qualified = found;

        return length;
      }

      if (enclosing.empty()) break;

      const auto dot = enclosing.rfind('.');
      enclosing = dot == std::string::npos ? "" : enclosing.substr(0, dot);
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
    return isType() && !next.string && next.text == ".";
  }
  bool declarationType() const {
    const auto length = typePath(pos);

    if (!length) return isType() && !typeMember();

    const auto &next = tokens.at(pos + length);
    return !next.string && (next.text == "*" || next.text == "&" || arraySuffix(pos + length) ||
           isIdentifierStart(next.text[0]));
  }
  // `[]`, `[N]`, `[-]`, or a size hint `[*]` or `[N*]` after an element type,
  // followed by a name or `&`.
  bool arraySuffix(size_t at) const {
    auto punct = [&](size_t k, const char *text) {
      return k < tokens.size() && !tokens[k].string && tokens[k].text == text;
    };

    if (!punct(at, "[")) return false;

    size_t close = at + 1;

    if (punct(close, "-")) ++close;
    else {
      if (close < tokens.size() && !tokens[close].string && std::isdigit(static_cast<unsigned char>(tokens[close].text[0])))
        ++close;

      if (punct(close, "*")) ++close;
    }

    if (!punct(close, "]") || close + 1 >= tokens.size()) return false;

    const auto &next = tokens[close + 1];
    // A following `[` is a further dimension, reported once the type is read.
    return !next.string && (next.text == "&" || next.text == "[" ||
                            isIdentifierStart(next.text[0]));
  }
  // iN and uN name an N-byte integer. Every i/u + digits word is reserved so
  // that out-of-range widths get a precise diagnostic.
  bool byteWidthType() const {
    const auto &t = peek().text;
    return !peek().string && t.size() >= 2 && (t[0] == 'i' || t[0] == 'u') &&
           std::all_of(t.begin() + 1, t.end(),
                       [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
  }
  bool isType() const {
    return byteWidthType() || at("byte") || at("uint") || at("uint8") || at("uint16") ||
           at("uint32") || at("uint64") ||
           at("int8") || at("short") || at("int16") ||
           at("int") || at("int32") || at("long") || at("int64") ||
           at("float") || at("double") || at("bool") || at("void") || at("cstring") ||
           at("string") || at("name") || at("char") || at("date") ||
           at("time") || at("datetime") || at("big") || at("ubig") || at("tiny") ||
           at("utiny") || at("func") || nativeMathType() || markupKeyword() || fileKeyword();
  }
  bool nativeMathType() const {
    const auto &token = tokens.at(pos);
    return !token.string && !token.character && nativeMath(token.text);
  }
  // `file` is a type only where one can be, as in `file log` or `file& log`:
  // it is still a usable name, as in `void* file`.
  bool fileKeyword() const {
    if (!at("file") || containerNames.count("file")) return false;

    const auto &next = tokens.at(pos + 1);
    return !next.string && !next.text.empty() && (isIdentifierStart(next.text[0]) || next.text == "&");
  }
  // `markup` is a type only where one can be: it is still a usable name.
  bool markupKeyword() const {
    if (!at("markup") || containerNames.count("markup")) return false;

    const auto &next = tokens.at(pos + 1);
    return !next.string && !next.text.empty() &&
           isIdentifierStart(next.text[0]);
  }
  // char(...), date(...), time(...), and datetime(...) construct validated values.
  bool constructorType() const {
    const auto &next = tokens.at(pos + 1);
    return (at("char") || at("date") || at("time") || at("datetime") || nativeMathType()) &&
           !next.string && next.text == "(";
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

        if (accept("-")) d.signedIndex = true;
        else if (accept("*")) d.sizeHint = Type::HugeHint;
        else if (!at("]")) {
          const auto count = pop();
          const bool hint = accept("*");

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

        expect("]");
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
    if (at(",")) {
      const auto saved = pos;
      pop();

      if (declarationType() || isType()) {
        const auto value = plainType();

        if (collectionWord() && peek().text == "pair") {
          pop();
          return Type::collection(Type::Pair, trackedElement(value), t);
        }
      }

      pos = saved;
    }

    if ((at("queue") || at("stack") || at("pair")) && arraySuffix(pos + 1))
      fail(peek(), "arrays of collections are not supported yet");

    // `T queue`, `T stack`, and `T pair` (keyed by name).
    if (collectionWord()) {
      const auto word = pop().text;

      if (word == "queue") return Type::collection(Type::Queue, trackedElement(t));
      if (word == "stack") return Type::collection(Type::Stack, trackedElement(t));

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

    while (accept("*")) t = t.pointerTo();
    if (t.depth && t.base == Type::Func) fail(peek(), "pointers to func values are unsupported");

    if (t.depth && (t.base == Type::String || t.base == Type::Name ||
                    t.base == Type::Big || t.base == Type::UBig))
      fail(peek(), "pointers to managed text or big integers are unsupported");

    return t;
  }
  // `queue`, `stack`, or `pair` after an element type, followed by the
  // declared name or a reference marker; elsewhere they are ordinary names.
  bool collectionWord() const {
    if (!at("queue") && !at("stack") && !at("pair")) return false;

    const auto &next = tokens.at(pos + 1);
    return !next.string && (next.text == "&" || isIdentifierStart(next.text[0]));
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

    if (accept("byte") || accept("uint8"))
      return Type::UInt8;

    if (accept("uint16"))
      return Type::UInt16;

    if (accept("uint") || accept("uint32"))
      return Type::UInt32;

    if (accept("uint64"))
      return Type::UInt64;

    if (accept("int8"))
      return Type::Int8;

    if (accept("short") || accept("int16"))
      return Type::Short;

    if (accept("long") || accept("int64"))
      return Type::Long;

    if (accept("float"))
      return Type::Float;

    if (accept("double"))
      return Type::Double;

    if (accept("int") || accept("int32"))
      return Type::Int;

    if (accept("bool"))
      return Type::Bool;

    if (markupKeyword()) {
      pop();
      return markupType();
    }

    if (fileKeyword()) {
      pop();
      return Type::File;
    }

    if (accept("string")) return Type::String;
    if (accept("name")) return Type::Name;
    if (accept("char")) return Type::Char;
    if (accept("date")) return Type::Date;
    if (accept("time")) return Type::Time;
    if (accept("datetime")) return Type::DateTime;
    if (accept("func")) return funcType();
    if (accept("big")) return Type::Big;
    if (accept("ubig")) return Type::UBig;
    if (accept("tiny")) return Type::Tiny;
    if (accept("utiny")) return Type::UTiny;

    if (accept("cstring"))
      return Type::Text;

    expect("void");
    return Type::Void;
  }
  // After `func`: a signature ahead, as in `func int(float)` or `func (int)`.
  bool funcSignatureAhead() {
    if (at("(")) return true;
    if (!isType() && !namedType()) return false;

    const auto saved = pos;
    plainType();
    const bool found = at("(");
    pos = saved;
    return found;
  }
  // `func`, or `func result(parameter types)`. Parameters may be named.
  Type funcType() {
    Type t(Type::Func);

    if (!funcSignatureAhead()) return t;

    auto signature = std::make_shared<FuncSignature>();

    if (!at("(")) signature->result = type();

    expect("(");
    lines();

    if (!at(")"))
      do {
        lines();
        Type parameter = type();

        if (parameter == Type::Void) fail(peek(), "parameter cannot have void type");

        requireFuncSignature(parameter, "a func parameter");
        bool reference = accept("&");

        if (!at(",") && !at(")") && !at("\n")) identifier();

        signature->parameters.push_back({parameter, reference});
        lines();
      } while (accept(","));

    expect(")");
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

    while (accept("[")) {
      do {
        lines();
        auto token = identifier();

        while (accept(".")) token.text += "." + identifier().text;

        tags.push_back({token});
        lines();
      } while (accept(","));

      expect("]");
      lines();
    }

    return tags;
  }
  // The parameter list of a function or lambda, from '(' through ')'.
  void parameterList(Function &f) {
    expect("(");
    lines();

    if (!at(")"))
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
        bool reference = accept("&");
        auto parameterName = identifier();
        parameterName.text = declareLocal(parameterName);
        f.params.push_back({parameterName, t, reference});
        lines();
      } while (accept(","));

    expect(")");
  }
  // `@ result(parameters) { body }`, after the '@'.
  void lambdaExpression(Expr &e) {
    e.kind = "lambda";
    Type result = Type::Void;

    if (!at("(")) result = type();

    requireFuncSignature(result, "a func result");
    Token name = e.token;
    name.text = "$lambda";
    Function f{name, result, {}, {}, false, {}};
    f.usings = usings;
    hygiene.emplace_back();
    parameterList(f);
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
  static int precedence(const Token &t) {
    if (t.string)
      return 0;

    const auto &s = t.text;

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
        (namedLength && tokens[pos + namedLength].text == "*") ||
        (namedLength && containerNames.count("#@" + named) && !tokens[pos + namedLength].string &&
         tokens[pos + namedLength].text == ")")) {
      e->kind = "cast";
      e->castType = type();
      lines();
      expect(")");
      e->args.push_back(expression(11, stopAtPipe));
    } else {
      e = expression();
      lines();

      if (at("::") && e->kind == ".") {
        // `(a.Child :: b)` gives false instead of trapping on an ownership
        // cycle, and leaves b's owner as it was.
        auto move = std::make_unique<Expr>();
        move->kind = "moveCheck";
        move->token = pop();
        move->args.push_back(std::move(e));
        move->args.push_back(expression());
        e = std::move(move);
        lines();
      }

      if (at("+=") || at("-=") || at("++") || at("--")) {
        // `(s += x)` gives the inserted index; `(s--)` and `(s -= [i])`
        // give the removed element.
        auto mutation = std::make_unique<Expr>();
        mutation->kind = "mutation";
        mutation->token = pop();
        mutation->args.push_back(std::move(e));

        if (mutation->token.text == "+=" || mutation->token.text == "-=")
          mutation->args.push_back(mutationOperand(mutation->token.text));

        e = std::move(mutation);
        lines();
      }

      expect(")");
    }
  }
  void braceLiteral(Expr &literal) {
    // A literal: `{X: 1}` initializes a container's fields, `{1, 2}` lists
    // values, and `{"a" : 1}` lists keyed entries. A bare name before ':'
    // is a field-style entry (its value first, then the name as written);
    // the expected type decides what the literal builds.
    literal.kind = "initializer";
    lines();

    while (!at("}")) {
      // A word right before ':' is a field-style key, even a keyword such
      // as `name` in {name: "Bob"}.
      const auto &next = tokens.at(pos + 1);
      std::unique_ptr<Expr> first;

      if (!peek().string && isIdentifierStart(peek().text[0]) &&
          !next.string && next.text == ":") {
        first = std::make_unique<Expr>();
        first->kind = "name";
        first->token = pop();
      } else
        first = expression();

      if (accept(":")) {
        auto entry = std::make_unique<Expr>();

        if (first->kind == "name") {
          entry->kind = "fieldInitializer";
          entry->token = first->token;
          entry->args.push_back(expression());
          entry->args.push_back(std::move(first));
        } else {
          entry->kind = "entry";
          entry->token = first->token;
          entry->args.push_back(std::move(first));
          entry->args.push_back(expression());
        }

        literal.args.push_back(std::move(entry));
      } else
        literal.args.push_back(std::move(first));

      if (!at("}") && !at("\n") && !at(",")) fail(peek(), "expected ',' or a new line between literal entries");

      accept(",");
      lines();
    }

    expect("}");
  }
  std::unique_ptr<Expr> primaryExpression(bool stopAtPipe, bool markupExpected) {
    auto e = std::make_unique<Expr>();
    e->token = peek();

    if (accept("$value")) {
      // An expanded value macro: its argument bindings, then its result.
      e->kind = "macroValue";
      hygiene.emplace_back();

      while (!accept("$result")) {
        e->bindings.push_back(simpleStatement());
        expect("\n");
      }

      e->args.push_back(expression());
      lines();
      expect("$end");
      hygiene.pop_back();
    } else if (accept("#")) {
      e->kind = "compileTime";
      e->args.push_back(expression(11, stopAtPipe));

      if (e->args[0]->kind != "call")
        fail(e->token, "'#' requires an explicit function call, as in #Call(1); "
                       "a constant declaration requires an explicit type");
    } else if (accept("-") || accept("!") || accept("~") || accept("&") || accept("*")) {
      e->kind = "unary";
      e->args.push_back(expression(11, stopAtPipe));
    } else if (accept("?")) {
      // `?p.q.Foo()` skips the call, arguments included, at the first invalid
      // link and reads as the result type's default.
      // `?a[i]` guards an index: a missing element skips a write and reads
      // as the element type's default. `?e..Foo()` skips a missing method.
      e->kind = "guard";
      e->args.push_back(expression(11, stopAtPipe));

      if (e->args[0]->kind != "index" && e->args[0]->kind != "duckCall" && e->args[0]->kind != "name" &&
          e->args[0]->kind != "." && !(e->args[0]->kind == "call" && e->args[0]->receiver))
        fail(e->token, "'?' guards an index such as ?a[i], a method call such as ?p.Foo(), "
                       "a duck call such as ?e..Foo(), or a scoped variable");
    } else if (at("@") && word(tokens.at(pos + 1)) && !punctuation(tokens.at(pos + 2), "(") &&
               !punctuation(tokens.at(pos + 2), "*") && !punctuation(tokens.at(pos + 2), "[") &&
               !punctuation(tokens.at(pos + 2), ".")) {
      // `@Name` asks whether that scope is active. A lambda's result type is
      // always followed by its parameter list.
      pop();
      e->kind = "scopeActive";
      e->token = identifier();
    } else if (at("{") && (markupExpected || markupAhead())) {
      markupLiteral(*e, nullptr);
    } else if (accept("@")) {
      lambdaExpression(*e);
    } else if (accept("(")) {
      parenthesized(e, stopAtPipe);
    } else if (accept("{")) {
      braceLiteral(*e);
    } else if (peek().character) {
      e->kind = "char";
      pop();
    } else if (peek().string) {
      e->kind = "text";
      pop();
    } else if (typeMember()) {
      e->kind = "typeMember";
      e->castType = baseType();
      expect(".");
      e->token = identifier();

      // vec3.Identity(): a function of the type rather than a limit like int.Max.
      if (nativeMath(e->castType) && accept("(")) {
        e->kind = "call";
        e->token.text = e->castType.record + "." + e->token.text;
        e->castType = Type();
        e->args = callArguments();
      }
    } else if (constructorType()) {
      e->kind = "construct";
      e->castType = baseType();
      expect("(");
      e->args = callArguments();
    } else if (accept("null")) {
      e->kind = "null";
    } else if (at("true") || at("false")) {
      e->kind = "bool";
      pop();
    } else if (std::isdigit(static_cast<unsigned char>(peek().text[0]))) {
      e->kind = integerLiteralBase(peek().text) != 10 ||
                        peek().text.find_first_of(".eE") == std::string::npos
                    ? "int" : "real";
      pop();
    } else {
      e->token = identifier();
      resolveLocal(e->token);
      e->kind = "name";
    }

    return e;
  }
  void duckCallSuffix(std::unique_ptr<Expr> &e) {
    // Dynamic dispatch: `e..Foo()`, and the checks `e..?Foo()` (would
    // this call run?) and `e..?Foo` (does any method of that name exist?).
    auto call = std::make_unique<Expr>();
    call->kind = accept("?") ? "duckCheck" : "duckCall";
    call->token = identifier();
    call->receiver = std::move(e);

    if (accept("(")) call->args = callArguments();
    else if (call->kind == "duckCheck") call->kind = "duckExists";
    else fail(call->token, "a duck call needs an argument list, as in e..Foo()");

    e = std::move(call);
  }
  void indexSuffix(std::unique_ptr<Expr> &e) {
    auto index = std::make_unique<Expr>();
    index->kind = "index";
    index->token = pop();
    index->args.push_back(std::move(e));
    lines();
    index->args.push_back(expression());
    lines();
    expect("]");
    e = std::move(index);
  }
  // The closing parenthesis of `a (0.5)== b`, or 0 if none is ahead. The
  // operator touches the `)`, and a space before the `(` tells it from the
  // call in `f(0.5)== b`.
  size_t toleranceClose() const {
    if (pos == 0 || !at("(")) return 0;

    const auto &before = tokens[pos - 1], &open = tokens[pos];

    if (before.line == open.line && before.column + int(before.text.size()) == open.column) return 0;

    int depth = 0;

    for (size_t k = pos; k + 1 < tokens.size(); ++k) {
      const auto &token = tokens[k];

      if (token.string) continue;
      if (token.text == "(") ++depth;

      if (token.text == ")" && --depth == 0) {
        const auto &next = tokens[k + 1];
        const bool comparison = !next.string && (next.text == "==" || next.text == "!=");
        return comparison && next.line == token.line && next.column == token.column + 1 ? k : 0;
      }
    }

    return 0;
  }
  void postfixChain(std::unique_ptr<Expr> &e) {
    std::string path = e->kind == "name" ? e->token.text : "";

    while (at(".") || (at("(") && !toleranceClose()) || at("[") || at("..")) {
      if (accept("..")) {
        duckCallSuffix(e);
        path.clear();
        continue;
      }

      if (at("[")) {
        indexSuffix(e);
        path.clear();
        continue;
      }

      if (accept(".")) {
        auto member = std::make_unique<Expr>();
        member->kind = ".";
        member->token = identifier();
        path += "." + member->token.text;
        member->args.push_back(std::move(e));
        e = std::move(member);
      } else {
        expect("(");

        if (e->kind == "call" || e->kind == "invoke" || e->kind == "index" || e->kind == "lambda" ||
            e->kind == "duckCall") {
          // The callee is itself a value, as in MakeAdder(1)(2).
          auto invoke = std::make_unique<Expr>();
          invoke->kind = "invoke";
          invoke->token = e->token;
          invoke->receiver = std::move(e);
          invoke->args = callArguments();
          e = std::move(invoke);
          path.clear();
          continue;
        }

        if (e->kind != "name" && e->kind != ".") fail(e->token, "expected a function or method name");

        if (e->kind == ".") {
          e->receiver = std::move(e->args.front());
          e->args.clear();
        }

        e->kind = "call";
        e->token.text = path;
        e->args = callArguments();
        path.clear();
      }
    }
  }
  // A node for the operator token at the cursor, with its left operand.
  std::unique_ptr<Expr> operatorNode(const char *kind, std::unique_ptr<Expr> left) {
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
        expect("(");
        auto tolerance = expression();
        expect(")");
        auto parent = operatorNode("binary", std::move(e));
        int p = precedence(parent->token);
        parent->args.push_back(expression(p + 1, stopAtPipe));
        parent->args.push_back(std::move(tolerance)); // A third operand marks a tolerance.
        e = std::move(parent);
        continue;
      }

      if (precedence(peek()) < minimum || (stopAtPipe && at("|"))) break;

      auto parent = operatorNode("binary", std::move(e));
      int p = precedence(parent->token);
      parent->args.push_back(expression(p + 1, stopAtPipe));
      e = std::move(parent);
    }

    // A range binds looser than every operator, so 0...a.Len-1 needs no
    // parentheses; a bare pipe after it belongs to the enclosing form.
    if (minimum == 1 && at("...")) {
      auto parent = operatorNode("range", std::move(e));
      parent->args.push_back(expression(2, true));
      e = std::move(parent);
    }

    if (minimum == 1 && at("?")) {
      auto parent = operatorNode("conditional", std::move(e));
      parent->args.push_back(expression(1, true));
      expect("|");
      parent->args.push_back(expression(1, stopAtPipe));
      e = std::move(parent);
    }

    return e;
  }
  std::vector<std::unique_ptr<Stmt>> block() {
    lines();
    expect("{");
    lines();
    hygiene.emplace_back();
    std::vector<std::unique_ptr<Stmt>> result;

    while (!at("}")) {
      if (at("<eof>")) expected("}");

      result.push_back(statement());

      if (!at("}") && !at("\n")) fail(peek(), "expected newline between statements");

      lines();
    }

    hygiene.pop_back();
    expect("}");
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
    e->kind = "name";
    return e;
  }
  static std::unique_ptr<Expr> memberOf(std::unique_ptr<Expr> base, const Token &member) {
    auto e = std::make_unique<Expr>();
    e->token = member;
    e->kind = ".";
    e->args.push_back(std::move(base));
    return e;
  }
  static std::unique_ptr<Stmt> assignment(std::unique_ptr<Expr> target, std::unique_ptr<Expr> value, const Token &at) {
    auto s = std::make_unique<Stmt>();
    s->token = at;
    s->kind = "assign";
    s->target = std::move(target);
    s->expr = std::move(value);
    return s;
  }
  // `{` that opens a markup literal: a block's first content is a tag.
  bool markupAhead() const {
    if (!at("{")) return false;

    size_t k = pos + 1;

    while (k < tokens.size() && punctuation(tokens[k], "\n")) ++k;

    return k + 1 < tokens.size() && punctuation(tokens[k], "<") && word(tokens[k + 1]);
  }
  bool closingTagAhead() const { return at("<") && punctuation(tokens.at(pos + 1), "/"); }
  // A value written directly inside an element, and the `value` field it needs.
  bool literalContent(Type &type) {
    const auto &t = peek();

    if (t.string) {
      type = t.character ? Type(Type::Char) : Type(Type::String);
      return true;
    }

    if (at("true") || at("false")) {
      type = Type::Bool;
      return true;
    }

    const auto &digit = at("-") ? tokens.at(pos + 1) : t;

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

    for (lines(); !at("}") && !closingTagAhead(); lines()) {
      if (at("<eof>")) fail(peek(), "unterminated markup: expected '}' or a closing tag");

      if (at("<")) {
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
        field.text = "value";
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
      splice->kind = "markupSplice";
      splice->target = holder(at);
      Token field = at;
      field.text = "value";
      field.string = field.character = false;
      splice->alternate.push_back(assignment(owner.empty() ? nullptr : memberOf(self(at), field), std::move(value), at));
      out.push_back(std::move(splice));
      children = true;

      if (root) root->markupClosed = false;
    }

    return children;
  }
  void closingTag(const std::string &tag, const std::string &written) {
    expect("<");
    expect("/");
    std::string closing;
    const auto closingAt = peek();
    const auto length = typePath(pos, &closing);

    if (!length || closing != tag)
      fail(closingAt, "expected closing tag </" + written + ">");

    pos += length;
    expect(">");
  }
  // `Name=value` pairs through the end of a tag. Values stop at any operator,
  // so `/>` is never read as a division: parenthesize an expression.
  void markupAttributes(Statements &out, const Place &self) {
    for (lines(); !at("/") && !at(">"); lines()) {
      const auto name = identifier();
      expect("=");
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
    group->kind = "block";
    auto own = std::make_unique<Stmt>();
    own->token = tagToken;
    own->token.text = local;
    own->kind = "own";
    own->name = local;
    Type record = Type::Record;
    record.record = tag;
    own->type = record.pointerTo();
    group->body.push_back(std::move(own));
    const Place self = [local](const Token &at) { return localName(at, local); };
    markupAttributes(group->body, self);

    if (accept("/")) expect(">");
    else {
      expect(">");
      Token field = tagToken;
      field.text = "$children";
      const Place children = [local, field](const Token &at) { return memberOf(localName(at, local), field); };
      tagToken.text = tag;

      if (markupItems(group->body, children, tag, tagToken, self, nullptr)) output->markupHosts.push_back(tagToken);

      closingTag(tag, written);
    }

    auto add = std::make_unique<Stmt>();
    add->token = open;
    add->kind = "markupAdd";
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
    e.kind = "macroValue";
    e.token = start;
    const auto local = "$markup" + std::to_string(markupLocals++);
    auto let = std::make_unique<Stmt>();
    let->token = start;
    let->token.text = local;
    let->kind = "let";
    let->name = local;
    let->type = markupType();
    e.bindings.push_back(std::move(let));
    const Place holder = [local](const Token &at) { return localName(at, local); };
    expect("{");
    lines();
    Token receiver = start;
    receiver.text = "this";
    const Place self = [](const Token &at) { return localName(at, "this"); };

    // A block's root may be written out; it is always the container itself.
    if (root && at("<") && tokens.at(pos + 1).text == bare && !punctuation(tokens.at(pos + 2), ".")) {
      pop();
      const auto rootName = pop();
      markupAttributes(e.bindings, self);

      if (accept("/")) expect(">");
      else {
        expect(">");
        markupItems(e.bindings, holder, tag, rootName, self, root);
        closingTag(tag, bare);
      }

      lines();
    } else
      markupItems(e.bindings, holder, tag, receiver, self, root);

    expect("}");
    auto take = std::make_unique<Expr>();
    take->token = start;
    take->token.text = local;
    take->kind = "markupTake";
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
    self.text = "this";
    Type type = Type::Record;
    type.record = record.name.text;
    f.params.push_back({self, type, true});
    record.markupClosed = true;
    auto result = std::make_unique<Stmt>();
    result->kind = "return";
    result->token = peek();
    result->expr = std::make_unique<Expr>();
    markupLiteral(*result->expr, &record, bare, record.name.text);
    f.body.push_back(std::move(result));
    program.push_back(std::move(f));
    auto call = std::make_unique<Expr>();
    call->kind = "call";
    call->token = name;
    call->token.text = "$self.$markup";
    call->receiver = localName(name, "$self");
    Token field = name;
    field.text = "$children";
    record.fields.push_back({field, markupType(), false, false, false, std::shared_ptr<Expr>(std::move(call))});
    record.fields.back().usings = usings;
  }
  // Whether `T.Name(` here is an assertion. A program's own container named
  // T keeps its members, so only the assertion names are claimed.
  bool assertionAhead() const {
    if (!at("T") || !punctuation(tokens.at(pos + 1), ".") || !word(tokens.at(pos + 2)) ||
        !punctuation(tokens.at(pos + 3), "("))
      return false;

    const bool container = containerNames.count("T") || containerNames.count("#{T");

    if (assertionNames().count(tokens[pos + 2].text)) return !currentTest.empty() || !container;

    return !currentTest.empty() && !container;
  }
  static const std::map<std::string, std::pair<size_t, size_t>> &assertionNames() {
    static const std::map<std::string, std::pair<size_t, size_t>> names{
        {"AreEqual", {2, 2}}, {"AreNotEqual", {2, 2}}, {"IsTrue", {1, 1}}, {"IsFalse", {1, 1}}, {"Fail", {0, 1}}};
    return names;
  }
  std::unique_ptr<Expr> syntheticExpr(const Token &start, const std::string &kind, const std::string &text) {
    auto e = std::make_unique<Expr>();
    e->token = start;
    e->token.text = text;
    e->kind = kind;
    return e;
  }
  std::unique_ptr<Expr> syntheticText(const Token &start, const std::string &value) {
    auto e = syntheticExpr(start, "text", value);
    e->token.string = true;
    return e;
  }
  std::unique_ptr<Expr> syntheticNot(const Token &start, std::unique_ptr<Expr> operand) {
    auto e = syntheticExpr(start, "unary", "!");
    e->args.push_back(std::move(operand));
    return e;
  }
  void assertionFailureStep(std::vector<std::unique_ptr<Stmt>> &failure, const Token &start, const std::string &op, std::unique_ptr<Expr> first, std::unique_ptr<Expr> second = nullptr) {
    auto call = syntheticExpr(start, "call", op);
    call->args.push_back(std::move(first));

    if (second) call->args.push_back(std::move(second));

    auto s = std::make_unique<Stmt>();
    s->token = start;
    s->kind = "expr";
    s->expr = std::move(call);
    failure.push_back(std::move(s));
  }
  std::unique_ptr<Expr> comparisonAssertion(const Token &start, const Token &check, std::vector<std::unique_ptr<Expr>> &arguments, std::vector<std::unique_ptr<Stmt>> &statements, std::vector<std::unique_ptr<Stmt>> &failure) {
    std::unique_ptr<Expr> condition;
    // A literal stays in place, so the comparison type-checks exactly as a
    // written `a == 5` would and `null` keeps the other operand's type.
    struct Operand {
      std::string kind;
      Token token;
    };
    auto operand = [&](std::unique_ptr<Expr> value, const std::string &local) {
      static const std::set<std::string> literals{"int", "real", "bool", "null", "text"};

      if (literals.count(value->kind)) return Operand{value->kind, value->token};

      auto let = std::make_unique<Stmt>();
      let->token = start;
      let->token.text = local;
      let->kind = "let";
      let->name = local;
      let->expr = std::move(value);
      statements.push_back(std::move(let));
      return Operand{"name", syntheticExpr(start, "name", local)->token};
    };
    auto read = [](const Operand &o) {
      auto e = std::make_unique<Expr>();
      e->kind = o.kind;
      e->token = o.token;
      return e;
    };
    const auto actual = operand(std::move(arguments[0]), "$actual");
    const auto expected = operand(std::move(arguments[1]), "$expected");
    auto equal = syntheticExpr(start, "binary", "==");
    equal->args.push_back(read(actual));
    equal->args.push_back(read(expected));
    const bool wantEqual = check.text == "AreEqual";
    condition = wantEqual ? syntheticNot(start, std::move(equal)) : std::move(equal);
    auto show = [&](const char *title, const Operand &o) {
      if (o.kind != "null") assertionFailureStep(failure, start, "$T.show", read(o), syntheticText(start, title));
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
    leave->kind = "return";
    failure.push_back(std::move(leave));
    auto test = std::make_unique<Stmt>();
    test->token = start;
    test->kind = "if";
    test->expr = std::move(condition);
    test->body = std::move(failure);

    if (statements.empty()) return test;

    statements.push_back(std::move(test));
    auto group = std::make_unique<Stmt>();
    group->token = start;
    group->kind = "block";
    group->body = std::move(statements);
    return group;
  }
  // An assertion lowers to ordinary statements: evaluate its operands once,
  // test them, and on failure report and return. Returning runs the test's
  // normal scope cleanup, so a failed test still releases what it holds.
  std::unique_ptr<Stmt> assertion() {
    const auto start = pop();
    expect(".");
    const auto check = identifier();
    const auto label = "T." + check.text;

    if (currentTest.empty())
      fail(start, label + " is available only directly inside a test in a T{ } block");

    const auto arity = assertionNames().find(check.text);

    if (arity == assertionNames().end())
      fail(check, "unknown assertion '" + label + "'; use AreEqual, AreNotEqual, IsTrue, IsFalse, or Fail");

    expect("(");
    auto arguments = callArguments();

    if (arguments.size() < arity->second.first || arguments.size() > arity->second.second)
      fail(start, label + " expects " + std::to_string(arity->second.second) + " argument" +
                      (arity->second.second == 1 ? "" : "s"));

    std::vector<std::unique_ptr<Stmt>> failure;
    assertionFailureStep(failure, start, "$T.fail", syntheticText(start, where(start) + ": test " + currentTest + ": " + label +
                              (check.text == "Fail" ? "" : " failed")));
    std::vector<std::unique_ptr<Stmt>> statements;
    std::unique_ptr<Expr> condition;

    if (arity->second.second == 2) {
      condition = comparisonAssertion(start, check, arguments, statements, failure);
    } else if (check.text == "Fail") {
      // Conditional in form only: statements after T.Fail stay reachable.
      condition = syntheticExpr(start, "bool", "true");

      if (!arguments.empty()) assertionFailureStep(failure, start, "$T.show", std::move(arguments[0]), syntheticText(start, "  message:  "));
    } else
      condition = check.text == "IsTrue" ? syntheticNot(start, std::move(arguments[0])) : std::move(arguments[0]);

    return assertionCheck(start, std::move(condition), std::move(failure), std::move(statements));
  }
  bool scopeMarkAhead() const { return at("[") && punctuation(tokens.at(pos + 1), "@"); }
  // `[@Name]` before a field or loop: the scope's name.
  Token scopeMark() {
    expect("[");
    expect("@");
    auto scope = identifier();
    expect("]");
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
    return at("@") && tokens.at(pos + 2).line == peek().line &&
           (tokens.at(pos + 1).text == "start" || tokens.at(pos + 1).text == "end") && !tokens.at(pos + 1).string &&
           word(tokens.at(pos + 2));
  }
  std::unique_ptr<Stmt> scopeStatement() {
    auto s = std::make_unique<Stmt>();
    s->token = pop();
    const bool starting = pop().text == "start";
    s->kind = starting ? "scopeStart" : "scopeEnd";

    if (!starting && lambdaDepth)
      fail(s->token, "@end leaves the functions up to its [@Name] loop, which a lambda body cannot do");

    s->name = identifier().text;

    if (!at("\n") && !at("}")) badStatementEnd();

    return s;
  }
  // `[@Name]` before a loop: each iteration's body runs inside the scope.
  std::unique_ptr<Stmt> scopedLoop() {
    const auto mark = scopeMark();

    if (!at("while") && !at("for") && !at("do"))
      fail(mark, "a scope mark in a function body applies to a loop");

    auto loop = statement();
    auto body = std::make_unique<Stmt>();
    body->token = mark;
    body->kind = "scopedBody";
    body->name = mark.text;
    body->body = std::move(loop->body);
    loop->body.clear();
    loop->body.push_back(std::move(body));
    return loop;
  }
  std::unique_ptr<Stmt> doStatement(std::unique_ptr<Stmt> s) {
    s->kind = "do";
    s->body = controlBody();
    // A `while` on the body's last line is its condition. One on a later
    // line is too, unless it has a block of its own: that is the next
    // statement, and this `do` has no condition.
    const auto end = pos;
    const bool sameLine = at("while");
    lines();

    if (accept("while")) {
      auto condition = expression();
      const bool terminated = at("\n") || at("}");

      if (sameLine && !terminated) fail(peek(), "expected terminator after do-while condition");

      const auto after = pos;
      lines();
      const bool ownBlock = at("{");
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
    s->kind = s->token.text;
    s->expr = expression();
    s->body = controlBody();

    if (s->kind == "if") {
      auto saved = pos;
      lines();

      if (accept("else")) {
        lines();

        if (at("if"))
          s->alternate.push_back(statement());
        else
          s->alternate = controlBody();
      } else
        pos = saved;
    }

    return s;
  }
  std::unique_ptr<Stmt> statement() {
    expandStatementMacro();

    if (scopeMarkAhead()) return scopedLoop();
    if (scopeStatementAhead()) return scopeStatement();
    if (at("Switch")) return switchStatement();

    if (at("macro") && word(tokens.at(pos + 1)) && punctuation(tokens.at(pos + 2), "("))
      fail(peek(), "a macro cannot be declared inside a function; declare it at file scope or in a container");

    if (at("case")) {
      const auto &next = tokens.at(pos + 1);

      if (next.string || next.text == "-" || word(next) || std::isdigit(static_cast<unsigned char>(next.text[0])))
        fail(peek(), "'case' is valid only directly inside a Switch body");
    }

    if (assertionAhead()) {
      auto check = assertion();

      if (!at("\n") && !at("}")) badStatementEnd();

      return check;
    }

    auto s = std::make_unique<Stmt>();
    s->token = peek();

    if (at("{")) {
      s->kind = "block";
      s->body = block();
      return s;
    }

    if (at("$") && punctuation(tokens.at(pos + 1), "{")) {
      pop();
      s->kind = "ordered";
      s->body = block();
      return s;
    }

    if (accept("do")) return doStatement(std::move(s));

    if (accept("for")) {
      hygiene.emplace_back();
      forClause(*s);
      hygiene.pop_back();
      return s;
    }

    if (accept("if") || accept("while")) return conditionalStatement(std::move(s));

    if (accept("break") || accept("continue"))
      s->kind = s->token.text;
    else if (accept("return")) {
      s->kind = "return";

      if (accept("::")) {
        returnsOwner = true;
        s->kind = "returnOwner";
      }

      if (!at("\n") && !at("}")) {
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

    if (!at("\n") && !at("}")) badStatementEnd(" (syntax may not be supported yet)");

    return s;
  }
  // One clause of a for loop and everything chained after it. `h | inner`
  // nests inner in h; `h | {after} inner` runs the body after the inner
  // loop and `h {before} | inner` before it. Each outer body shares one
  // statement list with its inner loop.
  void forClause(Stmt &s) {
    if (!forEachHeader(s)) {
      s.kind = "for";

      if (!at(";"))
        s.initializer = simpleStatement();

      expect(";");

      if (!at(";"))
        s.expr = expression();

      expect(";");

      if (!at("{") && !at("\n") && !at("|"))
        s.step = simpleStatement(true);

      if (s.step && (s.step->kind == "let" || s.step->kind == "own"))
        fail(s.step->token, "for step cannot declare a variable");
    }

    std::vector<std::unique_ptr<Stmt>> before, after;

    if (accept("|")) {
      lines();

      if (at("{")) {
        after = block();
        lines();
      }
    } else {
      before = controlBody();
      const auto end = pos;
      lines();

      if (!accept("|")) {
        pos = end;
        s.body = std::move(before);
        return;
      }

      lines();

      if (at("{")) fail(peek(), "an outer loop takes one body, before or after its inner loop");
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
    const bool reverse = accept("-");
    auto first = peek();
    const bool word = !first.string && !first.text.empty() &&
                      isIdentifierStart(first.text[0]);

    if (!word || at("<eof>") || (tokens[pos + 1].text != ":" && tokens[pos + 1].text != ",") ||
        tokens[pos + 1].string) {
      pos = start;
      return false;
    }

    auto valueName = identifier();
    std::optional<Token> keyName;

    if (accept(",")) {
      keyName = valueName;
      valueName = identifier();
    }

    expect(":");
    auto collection = expression(1, true);

    if (at(";") && !reverse && !keyName) {
      pos = start;
      return false;
    }

    // Declared after the collection, which still sees the outer names.
    s.name = declareLocal(valueName);

    if (keyName) s.key = declareLocal(*keyName);

    if (!at("{") && !at("\n") && !at("|"))
      fail(peek(), "expected '{' or a newline after the collection of a for loop");

    if (collection->kind == "range" && reverse)
      fail(collection->token, "a range already has a direction; write the high endpoint first, as 5...0");

    if (collection->kind == "range" && !s.key.empty())
      fail(collection->token, "a range has no keys; write for i : a...b");

    s.kind = collection->kind == "range" ? "rangefor" : "foreach";
    s.reverse = reverse;
    s.expr = std::move(collection);
    return true;
  }
  std::vector<std::unique_ptr<Stmt>> controlBody() {
    lines();

    if (at("{"))
      return block();

    std::vector<std::unique_ptr<Stmt>> body;
    body.push_back(statement());
    return body;
  }
  // The arguments of a call after its '(' has been read, through ')'.
  std::vector<std::unique_ptr<Expr>> callArguments() {
    std::vector<std::unique_ptr<Expr>> arguments;
    lines();

    if (!at(")"))
      do {
        lines();
        arguments.push_back(expression());
        lines();
      } while (accept(","));

    expect(")");
    return arguments;
  }
  // The right side of += or -=: `key : value` adds an entry to a pair, and
  // `[i]` or `[key]` after -= names the element to remove.
  std::unique_ptr<Expr> mutationOperand(const std::string &op, bool stopAtPipe = false) {
    if (op == "-=" && at("[")) {
      auto removal = std::make_unique<Expr>();
      removal->kind = "removal";
      removal->token = pop();
      lines();
      removal->args.push_back(expression());
      lines();
      expect("]");

      while (at("[")) {
        pop();
        lines();
        removal->args.push_back(expression());
        lines();
        expect("]");
      }

      return removal;
    }

    auto value = expression(1, stopAtPipe);

    if (op == "+=" && at(":")) {
      auto entry = std::make_unique<Expr>();
      entry->kind = "entry";
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
    if (accept("#")) {
      s->constant = declarationType();

      if (!s->constant) --pos;
    }

    if (s->constant && !declarationType())
      fail(peek(), "constant declaration requires an explicit type");

    if (declarationType()) {
      s->kind = "let";
      s->type = type();

      if (s->type == Type::Void)
        fail(s->token, "variable cannot have void type");

      const auto declaredName = identifier();
      s->name = declaredName.text;

      if (accept("(")) {
        // `Gun g(5)` and `Gun* p(5)` construct with arguments, like `Gun(5)`.
        auto arguments = callArguments();
        auto construction = [&](const Type &record) {
          auto call = std::make_unique<Expr>();
          call->kind = "call";
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

        s->kind = "own";

        if (!arguments.empty()) {
          if (!recordValue(s->type.pointee()))
            fail(s->token, "heap construction arguments require a container type");

          s->expr = construction(s->type.pointee());
        }
      } else if (s->constant) {
        if (!accept("="))
          fail(peek(), "constant '" + s->name + "' requires an initializer, as in #int age = 5");

        s->expr = expression();
      } else if (accept("=")) {
        expectMarkup = markupValue(s->type);
        s->expr = expression();
      }

      // Declared after the initializer, which still sees the outer names.
      s->name = declareLocal(declaredName);
    } else {
      unreachableNestedType();
      auto target = expression(1, stopAtPipe);

      if (at("::") && target->kind == ".") {
        pop();
        s->kind = "moveInto";
        s->target = std::move(target);
        s->expr = expression();
      } else if (at(":") || at("::")) {
        if (target->kind != "name") fail(target->token, "expected local name");

        s->kind = pop().text == "::" ? "own" : "let";
        s->expr = expression();
        s->name = declareLocal(target->token);
      } else if (at("=") || at("++") || at("--") || at("+=") || at("-=") ||
                 at("*=") || at("/=") || at("%=") || at("&=") || at("|=") ||
                 at("^=") || at("<<=") || at(">>=")) {
        auto op = pop().text;
        s->kind = op == "=" ? "assign" : op;
        s->target = std::move(target);

        if (op == "+=" || op == "-=") s->expr = mutationOperand(op, stopAtPipe);
        else if (op != "++" && op != "--") s->expr = expression(1, stopAtPipe);
      } else {
        s->kind = "expr";
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
      token.text = "this";
      f.params.push_back({token, self, true});
    }

    requireFuncSignature(f.result, "a func result");
    parameterList(f);

    if (external) {
      f.nativeSymbol = nativeName;

      if (accept("=")) {
        if (!peek().string || peek().character || peek().text.empty())
          fail(peek(), "native symbol must be a nonempty string literal");

        f.nativeSymbol = pop().text;
      }

      if (at("{"))
        fail(peek(), "native declaration cannot have a body");
    } else {
      returnsOwner = false;
      const bool outerMarkup = std::exchange(returnsMarkup, markupValue(f.result));
      f.body = block();
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

      if (punctuation(t, "<eof>")) return false;

      if (punctuation(t, "(")) ++depth;
      else if (punctuation(t, ")") && --depth == 0) return true;
    }
  }
  // After a pointer-typed member name: `Item* Child()` with no body is an
  // owning field, while `Item* Get() { ... }` is a method.
  bool owningFieldAhead(const Type &t) const {
    if (t.depth != 1 || !heapValue(t.pointee())) return false;

    auto punct = [&](size_t k, const char *text) { return !tokens.at(k).string && tokens.at(k).text == text; };
    size_t k = pos;

    if (!skipToCloseParen(k)) return false;
    for (++k; punct(k, "\n"); ++k) {}

    return !punct(k, "{");
  }
  // A '+' (public, the default) or '-' (protected) prefix must touch the
  // member it modifies, as in -int Cash or -Helper().
  bool accessModifier() {
    if (!at("+") && !at("-")) return false;

    const auto sign = pop();
    const auto &next = peek();

    if (next.string || next.text == "\n" || next.text == "<eof>" ||
        next.source != sign.source || next.line != sign.line || next.column != sign.column + 1)
      fail(sign, "access modifier '" + sign.text +
                     "' must be written directly before the member, without spaces");

    return sign.text == "-";
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

    if (!typed && (at(":") || enumBlock()))
      fail(name, "an enum belongs at file scope or in a static #{ } block, not in a container's instance body");

    if (!typed || t == Type::Void)
      fail(name, "container fields require a known value type");

    std::shared_ptr<Expr> initializer;

    if (at("(")) {
      // `Item* Child()` is an owning field built with its container.
      if (!recordValue(t.pointee()))
        fail(name, "field '" + name.text + "' cannot be constructed; write " + typeName(t.pointee()) + "* " +
                       name.text + " :: value");

      initializer = std::make_shared<Expr>();
      initializer->kind = "call";
      initializer->token = name;
      initializer->token.text = t.record;
      initializer->castType = t.pointee();
      expect("(");
      initializer->args = callArguments();
      t.role = Type::Owner;
    } else if (accept("::")) {
      // `Item* Child :: null` is an owning field that starts empty.
      if (t.depth != 1 || !heapValue(t.pointee()))
        fail(name, "an owning field needs a typed heap pointer");

      t.role = Type::Owner;

      if (!accept("null")) initializer = expression();
    } else {
      if (accept("=")) initializer = expression();
      // A pointer to a container is a tracked alias. void* and pointers
      // to scalars stay raw, in the container's C layout.
      if (t.depth == 1 && recordValue(t.pointee())) t.role = Type::Alias;
    }

    const bool lambdaDefault = initializer && initializer->kind == "lambda";

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
    if (!at("}") && !at("\n")) fail(peek(), "expected member terminator");

    lines();
  }
  // `bare` is the container's own name; the record carries its qualified one.
  void containerBody(Program &program, Record &record, std::set<std::string> &members,
                     const std::string &bare) {
    const auto &tag = record.name;
    const auto enclosing = scope;
    scope = tag.text;
    expect("{");
    lines();

    while (!accept("}")) {
      if (macroDeclaration()) {
        skipMacroDeclaration();
        continue;
      }

      if (at("this") && punctuation(tokens.at(pos + 1), "{")) {
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
      if (at("~")) {
        destructorDeclaration(program, members, bare, tag, tags, tagged, isProtected, modifier);
        endMember();
        continue;
      }

      bool typed = declarationType();
      Type t = typed ? type() : Type(Type::Void);
      auto name = identifier();

      // Name(...) declares a constructor, keyed by its parameter types;
      // -Name(...) declares a partial constructor.
      if (name.text == bare && at("(")) {
        constructorDeclaration(program, members, tag, name, typed, tags, tagged, isProtected);
        endMember();
        continue;
      }

      if (!members.insert(name.text).second) fail(name, "duplicate container member");
      if (name.text == "this") fail(name, "this is reserved for the receiver");

      if (at("(") && !(typed && owningFieldAhead(t))) {
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
    if (!at("T")) return false;

    const auto &next = tokens.at(pos + 1);
    return !next.string && next.text == "{" && next.source == peek().source && next.line == peek().line &&
           next.column == peek().column + 1;
  }
  // Each member is a test: `Name() { ... }`. Tests are static members of their
  // container, so they see what its other members see, protected ones too.
  void testBlock(Program &program, const std::string &container) {
    pop();
    const auto enclosing = scope;
    scope = container;
    expect("{");
    lines();

    while (!accept("}")) {
      if (macroDeclaration()) {
        skipMacroDeclaration();
        continue;
      }

      const auto start = peek();

      if (declarationType())
        fail(start, "a T{ } block holds only tests, written Name() { ... } with no result type");

      auto name = identifier();

      if (!at("(")) fail(name, "a T{ } block holds only tests, written Name() { ... }");

      auto key = name;
      key.text = container + ".$test$" + name.text;
      currentTest = container + "." + name.text;
      auto test = function(std::move(key), Type::Void, false, "");
      test.testName = std::exchange(currentTest, "");

      if (!test.params.empty()) fail(name, "a test takes no parameters");

      program.tests.push_back(std::move(test));

      if (!at("}") && !at("\n")) fail(peek(), "expected member terminator");

      lines();
    }

    scope = enclosing;
  }
  // `#{` starts a static body.
  bool staticBody() const {
    return at("#") && !tokens.at(pos + 1).string && tokens.at(pos + 1).text == "{";
  }
  // `@{` starts a getter block and `={` a setter block.
  bool accessorBody() const {
    return (at("@") || at("=")) && !tokens.at(pos + 1).string && tokens.at(pos + 1).text == "{";
  }
  // A getter block, `@{ Age: Years }`, or a setter block, `={ Age: Years =
  // value }`. Each entry declares one accessor of a property: an optional
  // type, the name, ':', and a getter expression or a setter statement, or a
  // `{ ... }` body. A setter reads the assigned value as `value`. Accessors
  // are instance methods named get-Name and set-Name.
  void accessorBlock(Program &program, const std::string &record, std::set<std::string> &members) {
    const bool getter = pop().text == "@";
    const auto enclosing = scope;
    scope = record;
    expect("{");
    lines();

    while (!accept("}")) {
      const bool isProtected = accessModifier();
      const bool typed = declarationType();
      const Type t = typed ? type() : Type(Type::Void);

      if (typed && t == Type::Void) fail(peek(), "a property requires a value type");

      auto name = identifier();

      if (name.text == "this") fail(name, "this is reserved for the receiver");

      expect(":");

      // A `{ ... }` body may start on the next line.
      if (const auto saved = pos; (lines(), !at("{"))) pos = saved;

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
      receiver.text = "this";
      f.params.push_back({receiver, self, true});

      if (getter) {
        if (at("{")) {
          if (!typed)
            fail(peek(), "a getter with a block body needs a type, as in int " + name.text + ": { ... }");

          f.body = block();
        } else {
          auto result = std::make_unique<Stmt>();
          result->kind = "return";
          result->token = peek();
          result->expr = expression();
          f.body.push_back(std::move(result));
        }
      } else {
        Token value = name;
        value.text = "value";
        f.params.push_back({value, t, false});

        if (at("{")) f.body = block();
        else f.body.push_back(statement());
      }

      program.push_back(std::move(f));

      if (!accept(",") && !at("}") && !at("\n")) fail(peek(), "expected newline, ',' or '}' after a property accessor");

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
        accept("\n");

        if (at("\n")) {
          lines();

          if (at("{") || staticBody() || accessorBody() || testBody())
            fail(peek(), "chained container bodies must be adjacent, without a blank line");
        }

        if (!at("{") && !staticBody() && !accessorBody() && !testBody()) {
          pos = saved;
          break;
        }
      }

      if (testBody()) {
        testBlock(program, qualified.text);
        continue;
      }

      if (accept("#")) {
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

    if (!at("<eof>") && !at("\n") && !(nested && at("}")))
      fail(peek(), "expected newline after container");

    lines();
  }

  // After a file-scope name, ':' starts either an enum (`Name : type #@{`)
  // or a parent list (`Name : Parent, Other {`). Whichever of '#' or '{'
  // comes first decides.
  bool parentList() const {
    if (!at(":")) return false;

    for (size_t i = pos + 1; i < tokens.size(); ++i) {
      const auto &t = tokens[i];

      if (t.string) continue;
      if (t.text == "{") return true;

      // An enum continues with `#@{`; a parent list may start with a static body.
      if (t.text == "#")
        return i + 1 < tokens.size() && !tokens[i + 1].string && tokens[i + 1].text == "{";

      if (t.text == "<eof>") return false;
    }

    return false;
  }
  bool enumBlock() const {
    return at("#") && !tokens.at(pos + 1).string && tokens.at(pos + 1).text == "@";
  }
  // Name [: type] #@{ Member[: literal], ... } with members separated by
  // commas or newlines. The type may be omitted on an extension fragment.
  void enumFragment(Program &program, Token tag) {
    EnumFragment fragment{tag, Type::Void, {}};

    if (accept(":")) {
      fragment.type = type();
      lines();
    }

    if (!enumBlock()) fail(peek(), "expected '#@{' after enum declaration");

    pop();
    pop();
    lines();
    expect("{");
    lines();

    while (!accept("}")) {
      EnumMember member{identifier(), nullptr};

      if (accept(":")) member.value = expression();

      fragment.members.push_back(std::move(member));

      if (!at("}") && !at("\n") && !at(",")) fail(peek(), "expected ',' or newline between enum members");

      accept(",");
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
    auto punct = [&](size_t k, const char *text) { return !tokens[k].string && tokens[k].text == text; };
    size_t k = pos;

    // A func result's own signature is not the parameter list.
    if (at("func")) {
      const auto saved = pos;
      ++pos;
      const bool signature = funcSignatureAhead();
      pos = saved;

      if (signature) {
        while (!punct(k, "(") && !punct(k, "<eof>") && !punct(k, "\n")) ++k;

        for (int depth = 0; !punct(k, "<eof>"); ++k) {
          if (punct(k, "(")) ++depth;
          else if (punct(k, ")") && --depth == 0) {
            ++k;
            break;
          }
        }
      }
    }

    for (; !punct(k, "("); ++k)
      if (tokens[k].string || punct(k, "<eof>") || punct(k, "\n") || punct(k, "=") || punct(k, ":") || punct(k, "::"))
        return false;

    if (!skipToCloseParen(k)) return false;

    for (++k; punct(k, "\n"); ++k) {
    }

    return punct(k, "{");
  }
  bool containerAhead() {
    // A static-only container is not a type name, but still has a body.
    const bool named = namedType();
    const auto saved = pos;
    ++pos;
    lines();
    const bool found = at("{") || staticBody() || (named && (parentList() || at(":") || enumBlock()));
    pos = saved;
    return found;
  }
  // Anything at file scope that is not a declaration is a statement. Tokens
  // that open no statement stay on the declaration path and its diagnostics.
  bool fileStatement() {
    if (at("extern") || at("{") || at("}") || at("+") || at("-")) return false;

    if (at("#")) {
      const auto &next = tokens.at(pos + 1);
      return next.string || (next.text != "{" && next.text != "@");
    }

    if (at("if") || at("for") || at("while") || at("do") || at("return") || at("break") || at("continue") ||
        at("Switch"))
      return true;

    return !functionAhead() && !containerAhead();
  }
  bool macroDeclaration() const {
    return at("macro") && word(tokens.at(pos + 1)) && punctuation(tokens.at(pos + 2), "(");
  }
  // collectMacros already captured it; a macro's body is checked only when used.
  void skipMacroDeclaration() {
    int depth = 0;
    pos += 2;

    for (;; ++pos) {
      if (at("(")) ++depth;
      else if (at(")") && --depth == 0) break;
    }

    ++pos;
    lines();
    depth = 0;

    for (;; ++pos) {
      if (at("{")) ++depth;
      else if (at("}") && --depth == 0) break;
    }

    ++pos;

    if (!at("\n") && !at("}") && !at("<eof>")) fail(peek(), "expected newline after macro");

    lines();
  }
  // Collects a file's statements into its hidden function, named by
  // combineUnits. Its locals live for one Exec; other files never see them.
  void fileScript(Program &program, std::vector<std::unique_ptr<Stmt>> body) {
    Token name = body.front()->token;
    name.text = "$exec.0";
    name.string = name.character = false;
    Function script{std::move(name), Type::Void, {}, std::move(body), false, {}};
    script.script = true;
    script.usings = usings;
    program.push_back(std::move(script));
  }

  bool attributeEnumMark(bool nested) {
    const auto opening = peek();
    const auto tags = attributes();

    if (tags.size() != 1 || tags[0].token.text != "args" || !namedType())
      fail(opening, nested ? "attributes on static members are not supported yet; only [args] can mark an enum here"
                           : "only [args] can precede a file-scope declaration, and it marks an enum");

    return true;
  }
  bool namedDeclaration(Program &functions, const std::string &prefix, bool nested, const Token &modifier, bool modified, bool attributeEnum) {
    auto saved = pos;
    auto tag = identifier();
    lines();

    if (parentList() || at("{") || staticBody()) {
      if (attributeEnum) fail(tag, "[args] marks an enum, not a container");
      if (modified) fail(modifier, "access modifiers on nested containers are not supported");

      std::vector<Token> parents;

      if (accept(":")) {
        do {
          lines();
          parents.push_back(parentName());
          lines();
        } while (accept(","));
      }

      containerChain(functions, tag, std::move(parents), prefix, nested);
      return true;
    }

    if (attributeEnum && !at(":") && !enumBlock()) fail(tag, "[args] marks an enum, not a container");

    if (at(":") || enumBlock()) {
      // An enum in a static body is named by its enclosing containers, as
      // in Outer.Day; inside Outer it is simply Day.
      tag.text = prefix + tag.text;
      enumFragment(functions, tag);
      functions.enums.back().attribute = attributeEnum;

      if (!at("<eof>") && !at("\n") && !(nested && at("}"))) fail(peek(), "expected newline after enum");

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

    if (accept("="))
      field.initializer = expression();
    else if (constant)
      fail(peek(), "constant '" + name.text + "' requires an initializer, as in #int Max = 10");

    if (scoped) {
      if (constant) fail(*scoped, "a constant cannot live in a scope");

      markScoped(field, *scoped, field.type, field.initializer != nullptr);
    }

    functions.statics.push_back(std::move(field));

    if (!at("\n") && !at("}")) fail(peek(), "expected newline after static field");

    lines();
  }
  void fileStatement(std::vector<std::unique_ptr<Stmt>> &statements) {
    statements.push_back(statement());

    if (!at("\n")) fail(peek(), "expected newline between statements");

    lines();
  }
  // With braced, reads one `{ ... }` static body; otherwise reads to <eof>.
  void declarations(Program &functions, const std::string &prefix,
                    bool nested, bool braced = false) {
    const auto enclosing = scope;
    const auto outer = usings;
    scope = prefix.empty() ? "" : prefix.substr(0, prefix.size() - 1);

    if (braced) expect("{");

    lines();
    usingDirectives();
    std::vector<std::unique_ptr<Stmt>> statements;

    while (!at("<eof>") && !(nested && at("}"))) {
      if (testBody())
        fail(peek(), "a T{ } test block must directly follow a container's body, with no blank line between them");

      if (at("using"))
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
      const bool attributeEnum = at("[") && attributeEnumMark(nested);

      if (!nested && !attributeEnum && fileStatement()) {
        fileStatement(statements);
        continue;
      }

      const auto modifier = peek();
      const bool modified = at("+") || at("-");

      if (modified && !nested) fail(modifier, "access modifiers apply only to container members");

      const bool isProtected = accessModifier();
      bool external = accept("extern");

      if (external) {
        if (!peek().string || peek().character || peek().text != "C")
          fail(peek(), "native declarations require extern \"C\"");

        pop();
      }

      if (!external && namedType() && namedDeclaration(functions, prefix, nested, modifier, modified, attributeEnum)) continue;

      // In a static block, `#int Max = 10` declares a constant static field.
      const auto constantToken = peek();
      const bool constant = nested && !external && accept("#");

      if (constant && !declarationType())
        fail(peek(), "constant static field requires an explicit type");

      bool typed = declarationType();
      Type result = typed ? type() : Type::Void;

      if (at("}") && !nested) fail(peek(), "unmatched '}': no block is open here");

      Token name = identifier();

      // `itn F() { ... }`: the word before the function name is not a type.
      if (!typed && !external && isWord(peek()) && !tokens.at(pos + 1).string && tokens.at(pos + 1).text == "(")
        unknownType(name);

      if (nested && !external && typed && !at("(")) {
        staticField(functions, name, result, prefix, constant, isProtected, scoped);
        continue;
      }

      if (scoped) fail(*scoped, "a scope mark applies to a field or a loop");
      if (constant) fail(constantToken, "'#' before a member declares a constant static field");

      if (!external && !typed && (at(":") || enumBlock()))
        fail(name, "enum declarations must be at file scope");

      if (!external && !typed && !at("(")) {
        lines();

        if (staticBody() || at("{")) {
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

      if (!nested && functions.back().name.text == "Main") entryParameter(functions.back());

      if (!at("<eof>") && !at("\n") && !(nested && at("}")))
        fail(peek(), "expected newline between functions");

      lines();
    }

    if (braced) expect("}");
    if (!statements.empty()) fileScript(functions, std::move(statements));

    scope = enclosing;
    usings = outer;
  }
  // `using Name.Space` lines at the top of a file or static body. The target
  // resolves like a type path, from the innermost container outward.
  void usingDirectives() {
    if (!at("using")) return;

    auto visible = *usings;

    while (at("using")) {
      const auto keyword = pop();
      auto start = peek();
      std::string path = identifier().text;

      while (accept(".")) path += "." + identifier().text;

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

      if (!at("\n") && !at("<eof>") && !at("}")) fail(peek(), "expected newline after using directive");

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
      : tokens(std::move(t)), containerNames(names), macros(macroTable) {}
  Program parse() {
    Program functions;
    output = &functions;
    functions.sources.push_back(tokens.front().source);
    // The last statement of a file need not end in a newline.
    tokens.insert(tokens.end() - 1, Token{"\n", tokens.back().line, tokens.back().column, false, tokens.back().source});
    declarations(functions, "", false);
    return functions;
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
bool punctuation(const std::vector<Token> &tokens, size_t k, const char *text) {
  return k < tokens.size() && !tokens[k].string && tokens[k].text == text;
}
// The '}' matching the '{' at open, or the end of the tokens.
size_t closingBrace(const std::vector<Token> &tokens, size_t open) {
  int nesting = 0;

  for (size_t k = open; k < tokens.size(); ++k) {
    if (tokens[k].string) continue;

    if (tokens[k].text == "{") ++nesting;
    else if (tokens[k].text == "}" && --nesting == 0) return k;
  }

  return tokens.size();
}
// Whether [k, end) starts with comma-separated dotted names leading to a
// body: a parent list, or an enum's underlying type.
bool namesBeforeBody(const std::vector<Token> &tokens, size_t k, size_t end) {
  auto skipLines = [&] { while (k < end && punctuation(tokens, k, "\n")) ++k; };
  auto word = [&] {
    return k < end && isWord(tokens[k]);
  };

  while (true) {
    skipLines();

    if (!word()) return false;

    for (++k; punctuation(tokens, k, ".") ; ++k) {
      ++k;

      if (!word()) return false;
    }

    skipLines();

    if (k < end && (punctuation(tokens, k, "{") || punctuation(tokens, k, "#"))) return true;
    if (!punctuation(tokens, k, ",")) return false;

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
    while (punctuation(tokens, k, "\n")) ++k;
    if (punctuation(tokens, k, ")")) break;

    MacroParameter parameter;
    int depth = 0;

    for (;; ++k) {
      if (k >= tokens.size() || punctuation(tokens, k, "<eof>")) fail(macro.name, "expected ')' after macro parameters");
      if (depth == 0 && (punctuation(tokens, k, ",") || punctuation(tokens, k, ")"))) break;

      if (punctuation(tokens, k, "(") || punctuation(tokens, k, "[")) ++depth;
      else if (punctuation(tokens, k, ")") || punctuation(tokens, k, "]")) --depth;

      if (!punctuation(tokens, k, "\n")) parameter.type.push_back(tokens[k]);
    }

    if (parameter.type.empty()) fail(tokens[k], "expected a macro parameter");

    parameter.name = parameter.type.back();
    parameter.type.pop_back();

    if (!isWord(k - 1) || parameter.type.empty())
      fail(parameter.name, "a macro parameter requires an explicit type and a name, as in int count");

    if (!parameter.type.back().string && parameter.type.back().text == "&")
      fail(parameter.name, "macro parameter '" + parameter.name.text + "' cannot be a reference: arguments are bound "
                           "once by value; a macro body can name the caller's variable directly");

    if (!seen.insert(parameter.name.text).second)
      fail(parameter.name, "duplicate macro parameter '" + parameter.name.text + "'");

    macro.parameters.push_back(std::move(parameter));

    if (punctuation(tokens, k, ",")) ++k;
  }

  ++k;

  while (punctuation(tokens, k, "\n")) ++k;
  if (!punctuation(tokens, k, "{")) fail(macro.name, "macro '" + macro.name.text + "' requires a { ... } body");

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
    if (t.text == "{") {
      i = closingBrace(tokens, i);
      continue;
    }

    if (!isIdentifierStart(t.text[0])) continue;

    if (t.text == "macro" && i + 2 < end && !tokens[i + 1].string && !tokens[i + 1].text.empty() &&
        (std::isalpha(static_cast<unsigned char>(tokens[i + 1].text[0])) || tokens[i + 1].text[0] == '_') &&
        punctuation(tokens, i + 2, "(")) {
      i = collectMacro(tokens, i, prefix, macros);
      continue;
    }

    // A file-scope statement declares no types, but its header can look as
    // if it does: `if ready {` and `for x : items {`. Skip the whole line.
    if (t.text == "if" || t.text == "else" || t.text == "for" || t.text == "while" || t.text == "do") {
      while (i < end && !punctuation(tokens, i, "\n")) {
        if (punctuation(tokens, i, "{")) i = closingBrace(tokens, i);

        ++i;
      }

      continue;
    }

    size_t next = i + 1;

    while (punctuation(tokens, next, "\n")) ++next;
    if (next >= end) continue;
    // `x : 1` is a statement; a declaration's ':' starts a list of names.
    if (punctuation(tokens, next, ":") && !namesBeforeBody(tokens, next + 1, end)) continue;

    size_t chain = next;

    // ':' after a name starts an enum's underlying type or a container's
    // parent list; an enum extension may omit it. A parent list reaches '{'
    // or '#{' before any other '#'. Skip the list or type so its words are
    // never taken as declared names.
    if (punctuation(tokens, next, ":") ||
        (punctuation(tokens, next, "#") && punctuation(tokens, next + 1, "@"))) {
      size_t stop = next;

      while (stop < end && (tokens[stop].string || (tokens[stop].text != "#" && tokens[stop].text != "{")))
        ++stop;

      const bool parents = punctuation(tokens, next, ":") && stop < end &&
                           (punctuation(tokens, stop, "{") ||
                            (punctuation(tokens, stop, "#") && punctuation(tokens, stop + 1, "{")));

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
      if (last != std::string::npos && punctuation(tokens, k, "T") && punctuation(tokens, k + 1, "{") &&
          tokens[k + 1].line == tokens[k].line && tokens[k + 1].column == tokens[k].column + 1) {
        const size_t close = closingBrace(tokens, k + 1);

        if (macros) collectScope(tokens, k + 2, close, prefix + t.text + ".", names, macros);

        last = close;
        k = close + 1;

        if (punctuation(tokens, k, "\n")) ++k;

        continue;
      }

      if (punctuation(tokens, k, "{")) open = k;
      else if (punctuation(tokens, k, "#") && punctuation(tokens, k + 1, "{")) open = k + 1;
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

      if (punctuation(tokens, k, "\n")) ++k;
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
} // namespace duck
