#include "Pipeline.h"
#include <cctype>
namespace duck {
namespace {
class Parser {
  std::vector<Token> tokens;
  size_t pos = 0;
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
  void separators() {
    while (accept("\n") || accept(";")) {
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
  bool isType() const {
    return at("byte") || at("uint") || at("uint8") || at("uint16") ||
           at("uint32") || at("uint64") ||
           at("int8") || at("short") || at("int16") ||
           at("int") || at("int32") || at("long") || at("int64") ||
           at("float") || at("double") || at("bool") || at("void") || at("cstring") ||
           at("string") || at("name");
  }
  Type type() {
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
    if (accept("cstring"))
      return Type::Text;
    expect("void");
    return accept("*") ? Type::Pointer : Type::Void;
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
    if (accept("-") || accept("!") || accept("~")) {
      e->kind = "unary";
      e->args.push_back(expression(11, stopAtPipe));
    } else if (accept("(")) {
      lines();
      if (isType()) {
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
    } else if (peek().string) {
      e->kind = "text";
      pop();
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
      while (accept("."))
        e->token.text += "." + identifier().text;
      if (accept("(")) {
        e->kind = "call";
        lines();
        if (!at(")"))
          do {
            lines();
            e->args.push_back(expression());
            lines();
          } while (accept(","));
        expect(")");
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
    separators();
    std::vector<std::unique_ptr<Stmt>> result;
    while (!at("}")) {
      if (at("<eof>"))
        fail(peek(), "expected '}'");
      result.push_back(statement());
      separators();
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
      if (!at("\n") && !at(";") && !at("}"))
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
      if (s->step && s->step->kind == "let")
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
      if (!at("\n") && !at(";") && !at("}"))
        s->expr = expression();
    } else
      s = simpleStatement();
    if (!at("\n") && !at(";") && !at("}"))
      fail(peek(), "expected newline, ';', or '}' after statement (syntax may "
                   "not be supported yet)");
    return s;
  }
  std::vector<std::unique_ptr<Stmt>> controlBody() {
    lines();
    if (at("{"))
      return block();
    std::vector<std::unique_ptr<Stmt>> body;
    body.push_back(statement());
    accept(";"); // A single statement's terminator precedes else/while.
    return body;
  }
  std::unique_ptr<Stmt> simpleStatement() {
    auto s = std::make_unique<Stmt>();
    s->token = peek();
    s->constant = accept("#");
    if (s->constant && !isType())
      fail(peek(), "constant declaration requires an explicit type");
    if (isType()) {
      s->kind = "let";
      s->type = type();
      if (s->type == Type::Void)
        fail(s->token, "variable cannot have void type");
      s->name = identifier().text;
      if (s->constant) {
        expect("=");
        s->expr = expression();
      } else if (accept("="))
        s->expr = expression();
    } else if (pos + 1 < tokens.size() && !peek().string &&
               !tokens[pos + 1].string &&
               (tokens[pos + 1].text == ":" || tokens[pos + 1].text == "=" ||
                tokens[pos + 1].text == "++" || tokens[pos + 1].text == "--" ||
                tokens[pos + 1].text == "+=" || tokens[pos + 1].text == "-=" ||
                tokens[pos + 1].text == "*=" || tokens[pos + 1].text == "/=" ||
                tokens[pos + 1].text == "%=" || tokens[pos + 1].text == "&=" ||
                tokens[pos + 1].text == "|=" || tokens[pos + 1].text == "^=" ||
                tokens[pos + 1].text == "<<=" || tokens[pos + 1].text == ">>=")) {
      s->name = identifier().text;
      auto op = pop().text;
      s->kind = op == ":" ? "let" : op == "=" ? "assign" : op;
      if (op != "++" && op != "--")
        s->expr = expression();
    } else {
      s->kind = "expr";
      s->expr = expression();
    }
    return s;
  }

  void declarations(Program &functions, const std::string &prefix,
                    bool nested) {
    separators();
    while (!at("<eof>") && !(nested && at("}"))) {
      bool external = accept("extern");
      if (external) {
        if (!peek().string || peek().text != "C")
          fail(peek(), "native declarations require extern \"C\"");
        pop();
      }
      bool typed = isType();
      Type result = typed ? type() : Type::Void;
      Token name = identifier();
      if (!external && !typed && !at("(")) {
        lines();
        if (at("{"))
          fail(name, "instance containers are not implemented yet; use #{ for "
                     "static functions");
        if (accept("#")) {
          const auto container = prefix + name.text + ".";
          do {
            lines();
            expect("{");
            declarations(functions, container, true);
            expect("}");
            // Chained blocks must be adjacent, with at most one newline.
            auto saved = pos;
            accept("\n");
            if (!at("#"))
              pos = saved;
          } while (accept("#"));
          if (!at("<eof>") && !at("\n") && !at(";") && !(nested && at("}")))
            fail(peek(), "expected newline after static container");
          separators();
          continue;
        }
      }
      name.text = prefix + name.text;
      auto nativeName = name.text.substr(prefix.size());
      Function f{std::move(name), result, {}, {}, {}};
      expect("(");
      lines();
      if (!at(")"))
        do {
          lines();
          if (!isType())
            fail(peek(),
                 "parameter requires a supported explicit type (int8, short, "
                 "int16, int, int32, long, int64, byte, uint, uint8, uint16, "
                 "uint32, uint64, float, double, bool, string, name, cstring, or void*)");
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
          if (!peek().string || peek().text.empty())
            fail(peek(), "native symbol must be a nonempty string literal");
          f.nativeSymbol = pop().text;
        }
        if (at("{"))
          fail(peek(), "native declaration cannot have a body");
      } else
        f.body = block();
      functions.push_back(std::move(f));
      if (!at("<eof>") && !at("\n") && !at(";") && !(nested && at("}")))
        fail(peek(), "expected newline between functions");
      separators();
    }
  }

public:
  explicit Parser(std::vector<Token> t) : tokens(std::move(t)) {}
  Program parse() {
    Program functions;
    declarations(functions, "", false);
    return functions;
  }
};
} // namespace
Program parse(std::vector<Token> tokens) {
  return Parser(std::move(tokens)).parse();
}
} // namespace duck
