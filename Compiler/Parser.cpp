#include "Pipeline.h"
#include <algorithm>
#include <cctype>
namespace duck {
namespace {
class Parser {
  std::vector<Token> tokens;
  size_t pos = 0;
  const std::set<std::string> &containerNames;
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
        t.text == "null")
      fail(t, "reserved keyword cannot be an identifier");
    return pop();
  }
  bool namedType() const {
    return !peek().string && containerNames.count(peek().text);
  }
  // int.Max, i3.Min, double.Max: a built-in type followed by a member.
  bool typeMember() const {
    const auto &next = tokens.at(pos + 1);
    return isType() && !next.string && next.text == ".";
  }
  bool declarationType() const {
    if (!namedType()) return isType() && !typeMember();
    const auto &next = tokens.at(pos + 1);
    return !next.string && (next.text == "*" || next.text == "&" ||
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
    Type t = baseType();
    while (accept("*")) t = t.pointerTo();
    if (t.depth && (t.base == Type::String || t.base == Type::Name ||
                    t.base == Type::Big || t.base == Type::UBig))
      fail(peek(), "pointers to managed text or big integers are unsupported");
    return t;
  }
  Type baseType() {
    if (namedType()) {
      Type t = containerNames.count("#@" + peek().text) ? Type::Enum : Type::Record;
      t.record = identifier().text;
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
    } else if (accept("(")) {
      lines();
      if ((isType() && !constructorType() && !typeMember()) ||
          (namedType() && tokens[pos + 1].text == "*")) {
        e->kind = "cast";
        e->castType = type();
        lines();
        expect(")");
        e->args.push_back(expression(11, stopAtPipe));
      } else {
        e = expression();
        lines();
        expect(")");
      }
    } else if (accept("{")) {
      e->kind = "initializer";
      lines();
      while (!at("}")) {
        auto field = std::make_unique<Expr>();
        field->token = identifier();
        field->kind = "fieldInitializer";
        expect(":");
        field->args.push_back(expression());
        e->args.push_back(std::move(field));
        if (!at("}") && !at("\n") && !at(",")) fail(peek(), "expected field separator");
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
    while (at(".") || at("(")) {
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
      lines();
      expect("while");
      s->expr = expression();
      if (!at("\n") && !at("}"))
        fail(peek(), "expected terminator after do-while condition");
      return s;
    }
    if (accept("for")) {
      s->kind = "for";
      if (!at(";"))
        s->initializer = simpleStatement();
      expect(";");
      if (!at(";"))
        s->expr = expression();
      expect(";");
      if (!at("{") && !at("\n"))
        s->step = simpleStatement();
      if (s->step && (s->step->kind == "let" || s->step->kind == "own"))
        fail(s->step->token, "for step cannot declare a variable");
      s->body = controlBody();
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
  std::vector<std::unique_ptr<Stmt>> controlBody() {
    lines();
    if (at("{"))
      return block();
    std::vector<std::unique_ptr<Stmt>> body;
    body.push_back(statement());
    return body;
  }
  std::unique_ptr<Stmt> simpleStatement() {
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
        expect(")");
        if (recordValue(s->type)) return s;
        if (s->type.depth != 1 || !heapValue(s->type.pointee()))
          fail(s->token, "heap construction requires a typed pointer");
        s->kind = "own";
      } else if (s->constant) {
        if (!accept("="))
          fail(peek(), "constant '" + s->name + "' requires an initializer, as in #int age = 5");
        s->expr = expression();
      } else if (accept("="))
        s->expr = expression();
    } else {
      auto target = expression();
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
        if (op != "++" && op != "--") s->expr = expression();
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
  void containerBody(Program &program, Record &record, std::set<std::string> &members) {
    const auto &tag = record.name;
    expect("{");
    lines();
    while (!accept("}")) {
      const bool isProtected = accessModifier();
      bool typed = declarationType();
      Type t = typed ? type() : Type(Type::Void);
      auto name = identifier();
      if (!members.insert(name.text).second) fail(name, "duplicate container member");
      if (name.text == "this") fail(name, "this is reserved for the receiver");
      if (at("(")) {
        if (name.text == tag.text) fail(name, "custom constructors are not implemented");
        name.text = methodKey(tag.text, name.text);
        program.push_back(function(std::move(name), t, false, "", tag.text));
        program.back().protectedAccess = isProtected;
      } else {
        if (!typed || t == Type::Void)
          fail(name, "container fields require a known value type");
        std::shared_ptr<Expr> initializer;
        if (accept("=")) initializer = expression();
        record.fields.push_back({name, t, false, isProtected, false, std::move(initializer)});
      }
      if (!at("}") && !at("\n")) fail(peek(), "expected member terminator");
      lines();
    }
  }
  // `#{` starts a static body.
  bool staticBody() const {
    return at("#") && !tokens.at(pos + 1).string && tokens.at(pos + 1).text == "{";
  }
  // A container name followed by adjacent bodies in any order: `{ ... }`
  // holds instance members and `#{ ... }` static ones. Bodies are separated
  // by at most one newline; a blank line ends the chain and is rejected.
  void containerChain(Program &program, const Token &tag, std::vector<Token> parents,
                      const std::string &prefix, bool nested) {
    Record record{tag, {}, std::move(parents)};
    std::set<std::string> members;
    bool instance = false;
    for (bool first = true;; first = false) {
      if (!first) {
        auto saved = pos;
        accept("\n");
        if (at("\n")) {
          lines();
          if (at("{") || staticBody())
            fail(peek(), "chained container bodies must be adjacent, without a blank line");
        }
        if (!at("{") && !staticBody()) {
          pos = saved;
          break;
        }
      }
      if (accept("#")) {
        declarations(program, prefix + tag.text + ".", true, true);
        continue;
      }
      if (nested) fail(tag, "instance containers must be declared at file scope");
      containerBody(program, record, members);
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
    if (braced) expect("{");
    lines();
    while (!at("<eof>") && !(nested && at("}"))) {
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
            if (nested) fail(tag, "instance containers must be declared at file scope");
            do {
              lines();
              parents.push_back(identifier());
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
  }

public:
  Parser(std::vector<Token> t, const std::set<std::string> &names) : tokens(std::move(t)), containerNames(names) {}
  Program parse() {
    Program functions;
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
void collectContainerNames(const std::vector<Token> &tokens, std::set<std::string> &names) {
  int depth = 0;
  for (size_t i = 0; i < tokens.size(); ++i) {
    const auto &t = tokens[i];
    if (t.string) continue;
    if (t.text == "{") ++depth;
    else if (t.text == "}") --depth;
    else if (depth == 0 && (std::isalpha(static_cast<unsigned char>(t.text[0])) || t.text[0] == '_')) {
      size_t next = i + 1;
      while (next < tokens.size() && tokens[next].text == "\n" && !tokens[next].string) ++next;
      if (next >= tokens.size() || tokens[next].string) continue;
      const auto &after = tokens[next].text;
      // A chain of `#{...}` bodies names a type once it reaches a `{...}` body.
      auto body = [&](size_t k) {
        if (k >= tokens.size() || tokens[k].string) return std::string::npos;
        if (tokens[k].text == "{") return k;
        if (tokens[k].text == "#" && k + 1 < tokens.size() && !tokens[k + 1].string &&
            tokens[k + 1].text == "{")
          return k + 1;
        return std::string::npos;
      };
      bool chainedInstance = false;
      for (size_t k = next, open; !chainedInstance && (open = body(k)) != std::string::npos;) {
        if (open == k) {
          chainedInstance = true;
          break;
        }
        int nesting = 0;
        size_t close = open;
        for (; close < tokens.size(); ++close) {
          if (tokens[close].string) continue;
          if (tokens[close].text == "{") ++nesting;
          else if (tokens[close].text == "}" && --nesting == 0) break;
        }
        k = close + 1;
        if (k < tokens.size() && !tokens[k].string && tokens[k].text == "\n") ++k;
      }
      if (chainedInstance)
        names.insert(t.text);
      // ':' after a file-scope name starts an enum's underlying type or a
      // container's parent list; an enum extension may omit it. A parent list
      // reaches '{' before any '#'. Skip the list or type so its words are
      // never taken as declared names.
      else if (after == ":" || (after == "#" && next + 1 < tokens.size() &&
                                !tokens[next + 1].string && tokens[next + 1].text == "@")) {
        size_t end = next;
        while (end < tokens.size() && (tokens[end].string || (tokens[end].text != "#" &&
               tokens[end].text != "{" && tokens[end].text != "<eof>")))
          ++end;
        names.insert(t.text);
        const bool parents = after == ":" && end < tokens.size() &&
            (tokens[end].text == "{" || (tokens[end].text == "#" && end + 1 < tokens.size() &&
                                         !tokens[end + 1].string && tokens[end + 1].text == "{"));
        if (!parents)
          names.insert("#@" + t.text);
        i = end - 1;
      }
    }
  }
}
Program parse(std::vector<Token> tokens, const std::set<std::string> &names) {
  return Parser(std::move(tokens), names).parse();
}
} // namespace duck
