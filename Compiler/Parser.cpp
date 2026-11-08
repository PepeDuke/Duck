#include "Pipeline.h"
#include <algorithm>
#include <cctype>
namespace duck {
namespace {
class Parser {
  std::vector<Token> tokens;
  size_t pos = 0;
  const std::set<std::string> &containerNames;
  // The qualified container whose declarations or member bodies are being
  // parsed, such as "Name.Space", or empty at file scope.
  std::string scope;
  // The using directives visible here, outermost scope first.
  Usings usings = std::make_shared<const std::vector<Using>>();
  bool returnsOwner = false;
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
  Token expect(const std::string &s) {
    if (!at(s))
      fail(peek(), "expected '" + s + "'");
    return pop();
  }
  void lines() {
    while (accept("\n")) {
    }
  }
  Token identifier() {
    auto t = peek();
    if (t.string || t.text.empty() ||
        !(std::isalpha(static_cast<unsigned char>(t.text[0])) ||
          t.text[0] == '_'))
      fail(t, "expected identifier");
    if (isType() || t.text == "true" || t.text == "false" ||
        t.text == "return" || t.text == "if" || t.text == "else" ||
        t.text == "while" || t.text == "for" || t.text == "break" ||
        t.text == "continue" || t.text == "do" || t.text == "extern" ||
        t.text == "null" || t.text == "using")
      fail(t, "reserved keyword cannot be an identifier");
    return pop();
  }
  // The tokens a container or enum type path at `at` spans, or 0. A path
  // such as `Space` or `Name.Space` resolves like a relative function name:
  // from the innermost enclosing container outward, then at file scope,
  // longest path first. `qualified` receives the type's full name.
  size_t typePath(size_t at, std::string *qualified = nullptr) const {
    std::vector<std::string> paths;
    for (size_t k = at;; k += 2) {
      const auto &t = tokens[k];
      if (t.string || t.text.empty() ||
          !(std::isalpha(static_cast<unsigned char>(t.text[0])) || t.text[0] == '_'))
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
             (std::isalpha(static_cast<unsigned char>(w.text[0])) || w.text[0] == '_');
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
           std::isalpha(static_cast<unsigned char>(next.text[0])) || next.text[0] == '_');
  }
  // `[]` or `[N]` after an element type, followed by a name or `&`.
  bool arraySuffix(size_t at) const {
    auto punct = [&](size_t k, const char *text) {
      return k < tokens.size() && !tokens[k].string && tokens[k].text == text;
    };
    if (!punct(at, "[")) return false;
    size_t close = at + 1;
    if (close < tokens.size() && !tokens[close].string &&
        (std::isdigit(static_cast<unsigned char>(tokens[close].text[0])) || tokens[close].text == "-"))
      ++close;
    if (!punct(close, "]") || close + 1 >= tokens.size()) return false;
    const auto &next = tokens[close + 1];
    // A following `[` is a further dimension, reported once the type is read.
    return !next.string && (next.text == "&" || next.text == "[" ||
                            std::isalpha(static_cast<unsigned char>(next.text[0])) || next.text[0] == '_');
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
           at("utiny");
  }
  // char(...), date(...), time(...), and datetime(...) construct validated values.
  bool constructorType() const {
    const auto &next = tokens.at(pos + 1);
    return (at("char") || at("date") || at("time") || at("datetime")) &&
           !next.string && next.text == "(";
  }
  Type type() {
    Type t = plainType();
    // `T [N]` is a fixed array of N elements and `T []` a dynamic array.
    // Further dimensions follow in index order: `T [5][10]` holds 5 arrays
    // of 10. `T [-][-]` is a sparse grid indexed by any int coordinates.
    if (arraySuffix(pos)) {
      struct Dimension {
        Token token;
        unsigned long long extent = 0;
        bool signedIndex = false;
      };
      std::vector<Dimension> dimensions;
      while (arraySuffix(pos)) {
        Dimension d{pop()};
        if (accept("-")) d.signedIndex = true;
        else if (!at("]")) {
          const auto count = pop();
          if (integerLiteralBase(count.text) != 10 ||
              count.text.find_first_not_of("0123456789") != std::string::npos)
            fail(count, "a fixed array's length must be a decimal integer literal");
          try {
            d.extent = std::stoull(count.text);
          } catch (const std::exception &) {
            fail(count, "fixed array length is too large");
          }
          if (d.extent == 0) fail(count, "a fixed array needs at least one element; use T [] for a dynamic array");
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
      for (size_t i = dimensions.size(); i-- > 0;) t = Type::array(t, dimensions[i].extent);
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
          return Type::collection(Type::Pair, value, t);
        }
      }
      pos = saved;
    }
    if ((at("queue") || at("stack") || at("pair")) && arraySuffix(pos + 1))
      fail(peek(), "arrays of collections are not supported yet");
    // `T queue`, `T stack`, and `T pair` (keyed by name).
    if (collectionWord()) {
      const auto word = pop().text;
      if (word == "queue") return Type::collection(Type::Queue, t);
      if (word == "stack") return Type::collection(Type::Stack, t);
      return Type::collection(Type::Pair, t, Type::Name);
    }
    return t;
  }
  Type plainType() {
    Type t = baseType();
    while (accept("*")) t = t.pointerTo();
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
    return !next.string && (next.text == "&" || std::isalpha(static_cast<unsigned char>(next.text[0])) ||
                            next.text[0] == '_');
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
    if (accept("string")) return Type::String;
    if (accept("name")) return Type::Name;
    if (accept("char")) return Type::Char;
    if (accept("date")) return Type::Date;
    if (accept("time")) return Type::Time;
    if (accept("datetime")) return Type::DateTime;
    if (accept("big")) return Type::Big;
    if (accept("ubig")) return Type::UBig;
    if (accept("tiny")) return Type::Tiny;
    if (accept("utiny")) return Type::UTiny;
    if (accept("cstring"))
      return Type::Text;
    expect("void");
    return Type::Void;
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
  std::unique_ptr<Expr> expression(int minimum = 1, bool stopAtPipe = false) {
    auto e = std::make_unique<Expr>();
    e->token = peek();
    if (accept("-") || accept("!") || accept("~") || accept("&") || accept("*")) {
      e->kind = "unary";
      e->args.push_back(expression(11, stopAtPipe));
    } else if (accept("?")) {
      // `?a[i]` guards an index: a missing element skips a write and reads
      // as the element type's default.
      e->kind = "guard";
      e->args.push_back(expression(11, stopAtPipe));
      if (e->args[0]->kind != "index")
        fail(e->token, "'?' guards an index such as ?a[i]; guarded member and call chains are not supported yet");
    } else if (accept("(")) {
      lines();
      if ((isType() && !constructorType() && !typeMember()) ||
          (namedType() && tokens[pos + typePath(pos)].text == "*")) {
        e->kind = "cast";
        e->castType = type();
        lines();
        expect(")");
        e->args.push_back(expression(11, stopAtPipe));
      } else {
        e = expression();
        lines();
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
    } else if (accept("{")) {
      // A literal: `{X: 1}` initializes a container's fields, `{1, 2}` lists
      // values, and `{"a" : 1}` lists keyed entries. A bare name before ':'
      // is a field-style entry (its value first, then the name as written);
      // the expected type decides what the literal builds.
      e->kind = "initializer";
      lines();
      while (!at("}")) {
        // A word right before ':' is a field-style key, even a keyword such
        // as `name` in {name: "Bob"}.
        const auto &next = tokens.at(pos + 1);
        std::unique_ptr<Expr> first;
        if (!peek().string && (std::isalpha(static_cast<unsigned char>(peek().text[0])) || peek().text[0] == '_') &&
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
          e->args.push_back(std::move(entry));
        } else
          e->args.push_back(std::move(first));
        if (!at("}") && !at("\n") && !at(",")) fail(peek(), "expected ',' or a new line between literal entries");
        accept(",");
        lines();
      }
      expect("}");
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
    } else if (constructorType()) {
      e->kind = "construct";
      e->castType = baseType();
      expect("(");
      lines();
      if (!at(")"))
        do {
          lines();
          e->args.push_back(expression());
          lines();
        } while (accept(","));
      expect(")");
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
      e->kind = "name";
    }
    std::string path = e->kind == "name" ? e->token.text : "";
    while (at(".") || at("(") || at("[")) {
      if (at("[")) {
        auto index = std::make_unique<Expr>();
        index->kind = "index";
        index->token = pop();
        index->args.push_back(std::move(e));
        lines();
        index->args.push_back(expression());
        lines();
        expect("]");
        e = std::move(index);
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
        if (e->kind != "name" && e->kind != ".") fail(e->token, "expected a function or method name");
        if (e->kind == ".") {
          e->receiver = std::move(e->args.front());
          e->args.clear();
        }
        e->kind = "call";
        e->token.text = path;
        lines();
        if (!at(")"))
          do {
            lines();
            e->args.push_back(expression());
            lines();
          } while (accept(","));
        expect(")");
        path.clear();
      }
    }
    // In a conditional's true arm, the first unparenthesized pipe is
    // its separator. Parentheses and call arguments start fresh expressions.
    while (precedence(peek()) >= minimum && !(stopAtPipe && at("|"))) {
      auto parent = std::make_unique<Expr>();
      parent->token = pop();
      parent->kind = "binary";
      int p = precedence(parent->token);
      parent->args.push_back(std::move(e));
      parent->args.push_back(expression(p + 1, stopAtPipe));
      e = std::move(parent);
    }
    // A range binds looser than every operator, so 0...a.Len-1 needs no
    // parentheses; a bare pipe after it belongs to the enclosing form.
    if (minimum == 1 && at("...")) {
      auto parent = std::make_unique<Expr>();
      parent->token = pop();
      parent->kind = "range";
      parent->args.push_back(std::move(e));
      parent->args.push_back(expression(2, true));
      e = std::move(parent);
    }
    if (minimum == 1 && at("?")) {
      auto parent = std::make_unique<Expr>();
      parent->token = pop();
      parent->kind = "conditional";
      parent->args.push_back(std::move(e));
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
    std::vector<std::unique_ptr<Stmt>> result;
    while (!at("}")) {
      if (at("<eof>"))
        fail(peek(), "expected '}'");
      result.push_back(statement());
      if (!at("}") && !at("\n")) fail(peek(), "expected newline between statements");
      lines();
    }
    expect("}");
    return result;
  }
  std::unique_ptr<Stmt> statement() {
    auto s = std::make_unique<Stmt>();
    s->token = peek();
    if (at("{")) {
      s->kind = "block";
      s->body = block();
      return s;
    }
    if (accept("do")) {
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
    if (accept("for")) {
      forClause(*s);
      return s;
    }
    if (accept("if") || accept("while")) {
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
    if (accept("break") || accept("continue"))
      s->kind = s->token.text;
    else if (accept("return")) {
      s->kind = "return";
      if (accept("::")) {
        returnsOwner = true;
        s->kind = "returnOwner";
      }
      if (!at("\n") && !at("}"))
        s->expr = expression();
    } else
      s = simpleStatement();
    if (!at("\n") && !at("}"))
      fail(peek(), "expected newline or '}' after statement (syntax may "
                   "not be supported yet)");
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
                      (std::isalpha(static_cast<unsigned char>(first.text[0])) || first.text[0] == '_');
    if (!word || at("<eof>") || (tokens[pos + 1].text != ":" && tokens[pos + 1].text != ",") ||
        tokens[pos + 1].string) {
      pos = start;
      return false;
    }
    s.name = identifier().text;
    if (accept(",")) {
      s.key = s.name;
      s.name = identifier().text;
    }
    expect(":");
    auto collection = expression(1, true);
    if (at(";") && !reverse && s.key.empty()) {
      s.name.clear();
      pos = start;
      return false;
    }
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
    s->constant = accept("#");
    if (s->constant && !declarationType())
      fail(peek(), "constant declaration requires an explicit type");
    if (declarationType()) {
      s->kind = "let";
      s->type = type();
      if (s->type == Type::Void)
        fail(s->token, "variable cannot have void type");
      s->name = identifier().text;
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
      } else if (accept("="))
        s->expr = expression();
    } else {
      unreachableNestedType();
      auto target = expression(1, stopAtPipe);
      if (at(":") || at("::")) {
        if (target->kind != "name") fail(target->token, "expected local name");
        s->kind = pop().text == "::" ? "own" : "let";
        s->name = target->token.text;
        s->expr = expression();
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
    expect("(");
    lines();
    if (!at(")"))
      do {
        lines();
        if (!declarationType())
          fail(peek(),
               "parameter requires a supported explicit type (int8, short, "
               "int16, int, int32, long, int64, byte, uint, uint8, uint16, "
               "uint32, uint64, i1-i64, u1-u64, big, ubig, tiny, utiny, float, double, bool, "
               "string, name, char, date, time, "
               "datetime, cstring, or void*)");
        Type t = type();
        if (t == Type::Void)
          fail(peek(), "parameter cannot have void type");
        bool reference = accept("&");
        f.params.push_back({identifier(), t, reference});
        lines();
      } while (accept(","));
    expect(")");
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
      f.body = block();
      if (returnsOwner) {
        if (f.result.depth != 1 || f.result == Type::Pointer)
          fail(f.name, "return :: requires a typed heap pointer result");
        f.result.role = Type::Owner;
      }
    }
    return f;
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
  // One instance body. Members accumulate across the chain's instance bodies.
  // `bare` is the container's own name; the record carries its qualified one.
  void containerBody(Program &program, Record &record, std::set<std::string> &members,
                     const std::string &bare) {
    const auto &tag = record.name;
    const auto enclosing = scope;
    scope = tag.text;
    expect("{");
    lines();
    while (!accept("}")) {
      const auto modifier = peek();
      const bool isProtected = accessModifier();
      // ~Name() declares the container's destructor.
      if (at("~")) {
        const auto tilde = pop();
        auto name = identifier();
        if (name.text != bare || tilde.line != name.line || tilde.column + 1 != name.column)
          fail(tilde, "a destructor is written ~" + bare + "()");
        if (isProtected) fail(modifier, "a destructor cannot be protected");
        if (!members.insert("~").second) fail(name, "container '" + tag.text + "' already declares a destructor");
        name.text = destructorKey(tag.text);
        program.push_back(function(std::move(name), Type::Void, false, "", tag.text));
        if (program.back().params.size() != 1) fail(tilde, "a destructor takes no parameters");
        if (!at("}") && !at("\n")) fail(peek(), "expected member terminator");
        lines();
        continue;
      }
      bool typed = declarationType();
      Type t = typed ? type() : Type(Type::Void);
      auto name = identifier();
      // Name(...) declares a constructor, keyed by its parameter types;
      // -Name(...) declares a partial constructor.
      if (name.text == bare && at("(")) {
        if (typed) fail(name, "a constructor has no result type");
        auto constructor = function(name, Type::Void, false, "", tag.text);
        const std::vector<Parameter> params(constructor.params.begin() + 1, constructor.params.end());
        constructor.name.text = constructorKey(tag.text, params, isProtected);
        if (!members.insert(constructor.name.text).second)
          fail(name, "container '" + tag.text + "' already declares a " +
                         (isProtected ? "partial " : "") + "constructor with these parameter types");
        program.push_back(std::move(constructor));
        if (!at("}") && !at("\n")) fail(peek(), "expected member terminator");
        lines();
        continue;
      }
      if (!members.insert(name.text).second) fail(name, "duplicate container member");
      if (name.text == "this") fail(name, "this is reserved for the receiver");
      if (at("(")) {
        name.text = methodKey(tag.text, name.text);
        program.push_back(function(std::move(name), t, false, "", tag.text));
        program.back().protectedAccess = isProtected;
      } else {
        if (!typed || t == Type::Void)
          fail(name, "container fields require a known value type");
        std::shared_ptr<Expr> initializer;
        if (accept("=")) initializer = expression();
        record.fields.push_back({name, t, false, isProtected, false, std::move(initializer)});
        record.fields.back().usings = usings;
      }
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
          if (at("{") || staticBody() || accessorBody())
            fail(peek(), "chained container bodies must be adjacent, without a blank line");
        }
        if (!at("{") && !staticBody() && !accessorBody()) {
          pos = saved;
          break;
        }
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

  // With braced, reads one `{ ... }` static body; otherwise reads to <eof>.
  void declarations(Program &functions, const std::string &prefix,
                    bool nested, bool braced = false) {
    const auto enclosing = scope;
    const auto outer = usings;
    scope = prefix.empty() ? "" : prefix.substr(0, prefix.size() - 1);
    if (braced) expect("{");
    lines();
    usingDirectives();
    while (!at("<eof>") && !(nested && at("}"))) {
      if (at("using"))
        fail(peek(), "using directives must come before the other declarations in their scope");
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
      if (!external && namedType()) {
        auto saved = pos;
        auto tag = identifier();
        lines();
        if (parentList() || at("{") || staticBody()) {
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
          continue;
        }
        if (at(":") || enumBlock()) {
          if (nested) fail(tag, "enum declarations must be at file scope");
          enumFragment(functions, tag);
          if (!at("<eof>") && !at("\n")) fail(peek(), "expected newline after enum");
          lines();
          continue;
        }
        pos = saved;
      }
      // In a static block, `#int Max = 10` declares a constant static field.
      const auto constantToken = peek();
      const bool constant = nested && !external && accept("#");
      if (constant && !declarationType())
        fail(peek(), "constant static field requires an explicit type");
      bool typed = declarationType();
      Type result = typed ? type() : Type::Void;
      Token name = identifier();
      if (nested && !external && typed && !at("(")) {
        if (result == Type::Void) fail(name, "static field cannot have void type");
        name.text = prefix + name.text;
        StaticField field{name, result, constant, isProtected, nullptr};
        field.usings = usings;
        if (accept("="))
          field.initializer = expression();
        else if (constant)
          fail(peek(), "constant '" + name.text + "' requires an initializer, as in #int Max = 10");
        functions.statics.push_back(std::move(field));
        if (!at("\n") && !at("}")) fail(peek(), "expected newline after static field");
        lines();
        continue;
      }
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
      if (!at("<eof>") && !at("\n") && !(nested && at("}")))
        fail(peek(), "expected newline between functions");
      lines();
    }
    if (braced) expect("}");
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
  Parser(std::vector<Token> t, const std::set<std::string> &names) : tokens(std::move(t)), containerNames(names) {}
  Program parse() {
    Program functions;
    functions.sources.push_back(tokens.front().source);
    declarations(functions, "", false);
    return functions;
  }
};
} // namespace
Program parse(std::vector<Token> tokens) {
  std::set<std::string> names;
  collectContainerNames(tokens, names);
  return parse(std::move(tokens), names);
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
// Collects the type names declared in [begin, end), one declaration level,
// qualified by prefix. Static bodies are scanned with the container's name
// appended to the prefix, so `Name #{ Space { } }` registers Name.Space.
void collectScope(const std::vector<Token> &tokens, size_t begin, size_t end,
                  const std::string &prefix, std::set<std::string> &names) {
  for (size_t i = begin; i < end; ++i) {
    const auto &t = tokens[i];
    if (t.string) continue;
    // Function bodies and other blocks declare no types.
    if (t.text == "{") {
      i = closingBrace(tokens, i);
      continue;
    }
    if (!(std::isalpha(static_cast<unsigned char>(t.text[0])) || t.text[0] == '_')) continue;
    size_t next = i + 1;
    while (punctuation(tokens, next, "\n")) ++next;
    if (next >= end) continue;
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
      if (punctuation(tokens, k, "{")) open = k;
      else if (punctuation(tokens, k, "#") && punctuation(tokens, k + 1, "{")) open = k + 1;
      else break;
      const size_t close = closingBrace(tokens, open);
      if (open == k) instance = true;
      else {
        names.insert("#{" + prefix + t.text);
        collectScope(tokens, open + 1, close, prefix + t.text + ".", names);
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
Program parse(std::vector<Token> tokens, const std::set<std::string> &names) {
  return Parser(std::move(tokens), names).parse();
}
} // namespace duck
