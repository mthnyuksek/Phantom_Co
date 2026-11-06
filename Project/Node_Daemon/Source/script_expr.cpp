#include "script_expr.hpp"

#include <cctype>
#include <vector>

namespace pc2::script {

enum class Kind { Lit, Var, Neg, Bin, Cmp, And, Or, Not, Call };

struct Node {
    Kind kind;
    size_t pos = 0;
    std::string name;  // variable, operator or function
    Value lit;
    std::vector<std::shared_ptr<const Node>> kids;
};

namespace {

using NodeP = std::shared_ptr<const Node>;

// ------------------------------------------------------------------ lexer
enum class Tok { Num, Time, Date, Str, Ident, Op, End };

struct Token {
    Tok kind;
    std::string text;  // literal text, identifier, operator; decoded text for Str
    size_t pos;
};

bool is_digit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }
bool is_ident_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool is_ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

std::vector<Token> lex(const std::string& s) {
    std::vector<Token> out;
    size_t k = 0;
    while (true) {
        while (k < s.size() && std::isspace(static_cast<unsigned char>(s[k]))) ++k;
        if (k >= s.size()) break;
        size_t start = k;
        char c = s[k];
        if (is_digit(c)) {
            size_t e = k;
            while (e < s.size() && is_digit(s[e])) ++e;
            size_t run = e - k;
            // 16:00
            if (run <= 2 && e + 2 < s.size() && s[e] == ':' && is_digit(s[e + 1]) && is_digit(s[e + 2]) &&
                (e + 3 >= s.size() || !is_digit(s[e + 3]))) {
                std::string t = s.substr(k, e + 3 - k);
                int64_t v;
                if (!parse_time_literal(t, v)) throw ScriptError("Bad time '" + t + "' (use 00:00 to 23:59)", start);
                out.push_back({Tok::Time, t, start});
                k = e + 3;
                continue;
            }
            // 2026-11-16
            if (run == 4 && e + 6 <= s.size() && s[e] == '-' && is_digit(s[e + 1]) && is_digit(s[e + 2]) && s[e + 3] == '-' &&
                is_digit(s[e + 4]) && is_digit(s[e + 5]) && (e + 6 == s.size() || !is_digit(s[e + 6]))) {
                std::string t = s.substr(k, 10);
                int64_t v;
                if (!parse_date_literal(t, v)) throw ScriptError("Bad date '" + t + "' (use YYYY-MM-DD)", start);
                out.push_back({Tok::Date, t, start});
                k = e + 6;
                continue;
            }
            if (e < s.size() && s[e] == '.' && e + 1 < s.size() && is_digit(s[e + 1])) {
                ++e;
                while (e < s.size() && is_digit(s[e])) ++e;
            }
            if (e < s.size() && is_ident_char(s[e])) throw ScriptError("Bad number", start);
            std::string t = s.substr(k, e - k);
            int64_t v;
            if (!parse_number_literal(t, v)) throw ScriptError("Bad number '" + t + "' (at most 2 decimals)", start);
            out.push_back({Tok::Num, t, start});
            k = e;
            continue;
        }
        if (c == '"') {
            std::string t;
            ++k;
            while (true) {
                if (k >= s.size()) throw ScriptError("Text without closing quote", start);
                char d = s[k++];
                if (d == '"') break;
                if (d == '\\') {
                    if (k >= s.size()) throw ScriptError("Text without closing quote", start);
                    char x = s[k++];
                    if (x == 'n') t += '\n';
                    else if (x == 't') t += '\t';
                    else if (x == '"' || x == '\\') t += x;
                    else throw ScriptError(std::string("Unknown escape \\") + x, k - 2);
                } else {
                    t += d;
                }
            }
            out.push_back({Tok::Str, t, start});
            continue;
        }
        if (is_ident_start(c)) {
            size_t e = k;
            while (e < s.size() && is_ident_char(s[e])) ++e;
            out.push_back({Tok::Ident, s.substr(k, e - k), start});
            k = e;
            continue;
        }
        static const char* two[] = {"<=", ">=", "==", "!=", "&&", "||"};
        bool done = false;
        for (const char* op : two) {
            if (s.compare(k, 2, op) == 0) {
                out.push_back({Tok::Op, op, start});
                k += 2;
                done = true;
                break;
            }
        }
        if (done) continue;
        if (std::string("+-*/%(),<>!").find(c) != std::string::npos) {
            out.push_back({Tok::Op, std::string(1, c), start});
            ++k;
            continue;
        }
        if (c == '=') throw ScriptError("Use == to compare", start);
        throw ScriptError(std::string("Unexpected character '") + c + "'", start);
    }
    out.push_back({Tok::End, "", s.size()});
    return out;
}

// ------------------------------------------------------------------ parser
class Parser {
public:
    explicit Parser(const std::string& s) : t_(lex(s)) {}

    NodeP parse() {
        if (peek().kind == Tok::End) throw ScriptError("Empty expression", 0);
        NodeP n = parse_or();
        if (peek().kind != Tok::End) throw ScriptError("Unexpected '" + peek().text + "'", peek().pos);
        return n;
    }

private:
    std::vector<Token> t_;
    size_t i_ = 0;

    const Token& peek() const { return t_[i_]; }
    bool is_op(const char* op) const { return peek().kind == Tok::Op && peek().text == op; }
    bool is_word(const char* w) const { return peek().kind == Tok::Ident && peek().text == w; }

    static NodeP make(Kind k, size_t pos, std::string name, std::vector<NodeP> kids = {}) {
        auto n = std::make_shared<Node>();
        n->kind = k;
        n->pos = pos;
        n->name = std::move(name);
        n->kids = std::move(kids);
        return n;
    }

    NodeP parse_or() {
        NodeP l = parse_and();
        while (is_word("or") || is_op("||")) {
            size_t p = peek().pos;
            ++i_;
            l = make(Kind::Or, p, "or", {l, parse_and()});
        }
        return l;
    }
    NodeP parse_and() {
        NodeP l = parse_not();
        while (is_word("and") || is_op("&&")) {
            size_t p = peek().pos;
            ++i_;
            l = make(Kind::And, p, "and", {l, parse_not()});
        }
        return l;
    }
    NodeP parse_not() {
        if (is_word("not") || is_op("!")) {
            size_t p = peek().pos;
            ++i_;
            return make(Kind::Not, p, "not", {parse_not()});
        }
        return parse_cmp();
    }
    bool at_cmp() const {
        return peek().kind == Tok::Op &&
               (peek().text == "<" || peek().text == "<=" || peek().text == ">" || peek().text == ">=" ||
                peek().text == "==" || peek().text == "!=");
    }
    NodeP parse_cmp() {
        NodeP l = parse_add();
        if (!at_cmp()) return l;
        Token op = peek();
        ++i_;
        NodeP n = make(Kind::Cmp, op.pos, op.text, {l, parse_add()});
        if (at_cmp()) throw ScriptError("Join comparisons with 'and'", peek().pos);
        return n;
    }
    NodeP parse_add() {
        NodeP l = parse_mul();
        while (is_op("+") || is_op("-")) {
            Token op = peek();
            ++i_;
            l = make(Kind::Bin, op.pos, op.text, {l, parse_mul()});
        }
        return l;
    }
    NodeP parse_mul() {
        NodeP l = parse_unary();
        while (is_op("*") || is_op("/") || is_op("%")) {
            Token op = peek();
            ++i_;
            l = make(Kind::Bin, op.pos, op.text, {l, parse_unary()});
        }
        return l;
    }
    NodeP parse_unary() {
        if (is_op("-")) {
            size_t p = peek().pos;
            ++i_;
            return make(Kind::Neg, p, "-", {parse_unary()});
        }
        return parse_primary();
    }
    NodeP parse_primary() {
        Token tk = peek();
        auto lit = [&](Value v) {
            ++i_;
            auto n = std::make_shared<Node>();
            n->kind = Kind::Lit;
            n->pos = tk.pos;
            n->lit = std::move(v);
            return NodeP(n);
        };
        int64_t v = 0;
        switch (tk.kind) {
            case Tok::Num: parse_number_literal(tk.text, v); return lit(Value::number(v));
            case Tok::Time: parse_time_literal(tk.text, v); return lit(Value::time(v));
            case Tok::Date: parse_date_literal(tk.text, v); return lit(Value::date(v));
            case Tok::Str: return lit(Value::text(tk.text));
            case Tok::Ident: {
                if (tk.text == "true") return lit(Value::boolean(true));
                if (tk.text == "false") return lit(Value::boolean(false));
                if (tk.text == "and" || tk.text == "or" || tk.text == "not")
                    throw ScriptError("Unexpected '" + tk.text + "'", tk.pos);
                ++i_;
                if (!is_op("(")) return make(Kind::Var, tk.pos, tk.text);
                ++i_;
                std::vector<NodeP> args;
                if (!is_op(")")) {
                    while (true) {
                        args.push_back(parse_or());
                        if (is_op(",")) { ++i_; continue; }
                        break;
                    }
                }
                if (!is_op(")")) throw ScriptError("Missing ')'", peek().pos);
                ++i_;
                return make(Kind::Call, tk.pos, tk.text, std::move(args));
            }
            case Tok::Op:
                if (tk.text == "(") {
                    ++i_;
                    NodeP n = parse_or();
                    if (!is_op(")")) throw ScriptError("Missing ')'", peek().pos);
                    ++i_;
                    return n;
                }
                throw ScriptError("Unexpected '" + tk.text + "'", tk.pos);
            default: throw ScriptError("Expression ends too early", tk.pos);
        }
    }
};

// ------------------------------------------------------------------ types
std::string tn(Type t) { return type_name(t); }

Type bin_type(const std::string& op, Type a, Type b) {
    using T = Type;
    if (op == "+") {
        if (a == T::Number && b == T::Number) return T::Number;
        if ((a == T::Time && b == T::Number) || (a == T::Number && b == T::Time)) return T::Time;
        if ((a == T::Date && b == T::Number) || (a == T::Number && b == T::Date)) return T::Date;
        if (a == T::Text && b == T::Text) return T::Text;
    } else if (op == "-") {
        if (a == T::Number && b == T::Number) return T::Number;
        if (a == T::Time && b == T::Number) return T::Time;
        if (a == T::Time && b == T::Time) return T::Number;
        if (a == T::Date && b == T::Number) return T::Date;
        if (a == T::Date && b == T::Date) return T::Number;
    } else if (a == T::Number && b == T::Number) {
        return T::Number;  // * / %
    }
    return T::Invalid;
}

bool cmp_ok(const std::string& op, Type a, Type b) {
    if (a != b || a == Type::Invalid) return false;
    if (a == Type::Bool) return op == "==" || op == "!=";
    return true;
}

Type call_type(const Node& n, const std::vector<Type>& args) {
    auto want = [&](size_t count) {
        if (args.size() != count)
            throw ScriptError(n.name + "() needs " + std::to_string(count) + (count == 1 ? " value" : " values"), n.pos);
    };
    if (n.name == "abs" || n.name == "round") {
        want(1);
        if (args[0] != Type::Number) throw ScriptError(n.name + "() needs a Number, got " + tn(args[0]), n.pos);
        return Type::Number;
    }
    if (n.name == "min" || n.name == "max") {
        want(2);
        if (args[0] != args[1] || (args[0] != Type::Number && args[0] != Type::Time && args[0] != Type::Date))
            throw ScriptError(n.name + "() needs two Numbers, Times or Dates, got " + tn(args[0]) + " and " + tn(args[1]), n.pos);
        return args[0];
    }
    throw ScriptError("Unknown function '" + n.name + "'", n.pos);
}

Type check(const Node& n, const TypeEnv& env) {
    switch (n.kind) {
        case Kind::Lit: return n.lit.type;
        case Kind::Var: {
            auto it = env.find(n.name);
            if (it == env.end()) throw ScriptError("Unknown variable '" + n.name + "'", n.pos);
            return it->second;
        }
        case Kind::Neg: {
            Type t = check(*n.kids[0], env);
            if (t != Type::Number) throw ScriptError("'-' needs a Number, got " + tn(t), n.pos);
            return Type::Number;
        }
        case Kind::Bin: {
            Type a = check(*n.kids[0], env), b = check(*n.kids[1], env);
            Type r = bin_type(n.name, a, b);
            if (r == Type::Invalid) throw ScriptError("Cannot use " + tn(a) + " " + n.name + " " + tn(b), n.pos);
            return r;
        }
        case Kind::Cmp: {
            Type a = check(*n.kids[0], env), b = check(*n.kids[1], env);
            if (!cmp_ok(n.name, a, b)) throw ScriptError("Cannot compare " + tn(a) + " " + n.name + " " + tn(b), n.pos);
            return Type::Bool;
        }
        case Kind::And: case Kind::Or: case Kind::Not:
            for (const auto& k : n.kids) {
                Type t = check(*k, env);
                if (t != Type::Bool) throw ScriptError("'" + n.name + "' needs Bool values, got " + tn(t), n.pos);
            }
            return Type::Bool;
        case Kind::Call: {
            std::vector<Type> args;
            for (const auto& k : n.kids) args.push_back(check(*k, env));
            return call_type(n, args);
        }
    }
    return Type::Invalid;
}

// ------------------------------------------------------------------ evaluation
int64_t whole(int64_t hundredths, const char* unit, size_t pos) {
    if (hundredths % 100 != 0) throw ScriptError(std::string("Use whole ") + unit, pos);
    return hundredths / 100;
}

Value date_checked(int64_t days, size_t pos) {
    int y, m, d;
    civil_from_days(days, y, m, d);
    if (days < days_from_civil(kMinYear, 1, 1) || days > days_from_civil(kMaxYear, 12, 31))
        throw ScriptError("Date out of range", pos);
    return Value::date(days);
}

Value time_wrapped(int64_t minutes) { return Value::time(((minutes % 1440) + 1440) % 1440); }

Value eval(const Node& n, const ValueEnv& env) {
    switch (n.kind) {
        case Kind::Lit: return n.lit;
        case Kind::Var: {
            auto it = env.find(n.name);
            if (it == env.end()) throw ScriptError("Unknown variable '" + n.name + "'", n.pos);
            return it->second;
        }
        case Kind::Neg: {
            Value v = eval(*n.kids[0], env);
            if (v.type != Type::Number) throw ScriptError("'-' needs a Number, got " + tn(v.type), n.pos);
            try { return Value::number(num_sub(0, v.i)); } catch (const ScriptError& e) { throw ScriptError(e.what(), n.pos); }
        }
        case Kind::And: case Kind::Or: {
            Value a = eval(*n.kids[0], env);
            if (a.type != Type::Bool) throw ScriptError("'" + n.name + "' needs Bool values, got " + tn(a.type), n.pos);
            if (n.kind == Kind::And && !a.b) return Value::boolean(false);
            if (n.kind == Kind::Or && a.b) return Value::boolean(true);
            Value b = eval(*n.kids[1], env);
            if (b.type != Type::Bool) throw ScriptError("'" + n.name + "' needs Bool values, got " + tn(b.type), n.pos);
            return Value::boolean(b.b);
        }
        case Kind::Not: {
            Value a = eval(*n.kids[0], env);
            if (a.type != Type::Bool) throw ScriptError("'not' needs a Bool value, got " + tn(a.type), n.pos);
            return Value::boolean(!a.b);
        }
        case Kind::Cmp: {
            Value a = eval(*n.kids[0], env), b = eval(*n.kids[1], env);
            if (!cmp_ok(n.name, a.type, b.type)) throw ScriptError("Cannot compare " + tn(a.type) + " " + n.name + " " + tn(b.type), n.pos);
            int c = 0;
            if (a.type == Type::Text) c = a.s < b.s ? -1 : (a.s > b.s ? 1 : 0);
            else if (a.type == Type::Bool) c = a.b == b.b ? 0 : 1;
            else c = a.i < b.i ? -1 : (a.i > b.i ? 1 : 0);
            const std::string& op = n.name;
            bool r = op == "<" ? c < 0 : op == "<=" ? c <= 0 : op == ">" ? c > 0 : op == ">=" ? c >= 0 : op == "==" ? c == 0 : c != 0;
            return Value::boolean(r);
        }
        case Kind::Bin: {
            Value a = eval(*n.kids[0], env), b = eval(*n.kids[1], env);
            const std::string& op = n.name;
            if (bin_type(op, a.type, b.type) == Type::Invalid)
                throw ScriptError("Cannot use " + tn(a.type) + " " + op + " " + tn(b.type), n.pos);
            try {
                if (a.type == Type::Text) return Value::text(a.s + b.s);
                if (a.type == Type::Number && b.type == Type::Number) {
                    if (op == "+") return Value::number(num_add(a.i, b.i));
                    if (op == "-") return Value::number(num_sub(a.i, b.i));
                    if (op == "*") return Value::number(num_mul(a.i, b.i));
                    if (op == "/") return Value::number(num_div(a.i, b.i));
                    return Value::number(num_mod(a.i, b.i));
                }
                // minutes / days between two Times / Dates (small numbers: no overflow)
                if (a.type == b.type) return Value::number((a.i - b.i) * 100);
                // Time/Date with a Number of minutes/days
                const Value& base = a.type == Type::Number ? b : a;
                const Value& amount = a.type == Type::Number ? a : b;
                int64_t k = whole(amount.i, base.type == Type::Time ? "minutes" : "days", n.pos);
                if (op == "-") k = -k;
                if (base.type == Type::Time) return time_wrapped(base.i + k % 1440);
                if (k > 4000000 || k < -4000000) throw ScriptError("Date out of range", n.pos);
                return date_checked(base.i + k, n.pos);
            } catch (const ScriptError& e) {
                throw ScriptError(e.what(), e.pos == std::string::npos ? n.pos : e.pos);
            }
        }
        case Kind::Call: {
            std::vector<Value> args;
            std::vector<Type> types;
            for (const auto& k : n.kids) {
                args.push_back(eval(*k, env));
                types.push_back(args.back().type);
            }
            call_type(n, types);
            try {
                if (n.name == "abs") return Value::number(args[0].i < 0 ? num_sub(0, args[0].i) : args[0].i);
                if (n.name == "round") return Value::number(num_round(args[0].i));
                bool first = n.name == "min" ? args[0].i <= args[1].i : args[0].i >= args[1].i;
                return first ? args[0] : args[1];
            } catch (const ScriptError& e) {
                throw ScriptError(e.what(), n.pos);
            }
        }
    }
    throw ScriptError("Bad expression", n.pos);
}

void collect_vars(const Node& n, std::set<std::string>& out) {
    if (n.kind == Kind::Var) out.insert(n.name);
    for (const auto& k : n.kids) collect_vars(*k, out);
}

json error_json(const char* stage, const ScriptError& e) {
    json j = {{"ok", false}, {"stage", stage}, {"error", e.what()}};
    j["pos"] = e.pos == std::string::npos ? json(nullptr) : json(e.pos);
    return j;
}

}  // namespace

Expr parse_expr(const std::string& src) {
    if (src.size() > 4000) throw ScriptError("Expression too long", 0);
    Parser p(src);
    return Expr{p.parse(), src};
}

Type check_expr(const Expr& e, const TypeEnv& vars) {
    if (!e.root) throw ScriptError("Empty expression", 0);
    return check(*e.root, vars);
}

Value eval_expr(const Expr& e, const ValueEnv& vars) {
    if (!e.root) throw ScriptError("Empty expression", 0);
    return eval(*e.root, vars);
}

std::set<std::string> expr_vars(const Expr& e) {
    std::set<std::string> out;
    if (e.root) collect_vars(*e.root, out);
    return out;
}

std::string script_eval_line(const std::string& line) {
    json req = json::parse(line, nullptr, false);
    if (!req.is_object()) return dumps({{"ok", false}, {"stage", "request"}, {"error", "Bad JSON"}});
    auto answer = [](const Value& v) {
        json j = {{"ok", true}, {"type", type_name(v.type)}, {"text", format_value(v, "en")}, {"text_tr", format_value(v, "tr")}};
        j["value"] = value_to_json(v)["value"];
        return dumps(j);
    };
    if (req.contains("loose")) {
        json l = req["loose"];
        Value v;
        if (!l.is_object() || !parse_value_loose(type_from_name(l.value("type", "")), l.value("text", ""), v))
            return dumps({{"ok", false}, {"stage", "parse"}, {"error", "Bad value"}});
        return answer(v);
    }
    TypeEnv types;
    ValueEnv values;
    json vars = req.value("vars", json::object());
    for (auto& [name, spec] : vars.items()) {
        Type t = spec.is_object() ? type_from_name(spec.value("type", "")) : Type::Invalid;
        Value v;
        if (t == Type::Invalid || !parse_value(t, spec.value("value", ""), v))
            return dumps({{"ok", false}, {"stage", "vars"}, {"error", "Bad variable '" + name + "'"}});
        types[name] = t;
        values[name] = v;
    }
    Expr e;
    try { e = parse_expr(req.value("expr", "")); } catch (const ScriptError& x) { return dumps(error_json("parse", x)); }
    Type t;
    try { t = check_expr(e, types); } catch (const ScriptError& x) { return dumps(error_json("type", x)); }
    Value v;
    try { v = eval_expr(e, values); } catch (const ScriptError& x) { return dumps(error_json("eval", x)); }
    if (v.type != t) return dumps({{"ok", false}, {"stage", "eval"}, {"error", "Type check and result differ"}});
    return answer(v);
}

}  // namespace pc2::script
