#include "script_value.hpp"

#include <cctype>
#include <limits>

namespace pc2::script {

namespace {

constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
constexpr int64_t kMin = std::numeric_limits<int64_t>::min();

int64_t to_i64(__int128 v) {
    if (v > kMax || v < kMin) throw ScriptError("Number too big");
    return static_cast<int64_t>(v);
}

// n / d rounded half away from zero (d != 0)
__int128 div_round(__int128 n, __int128 d) {
    __int128 q = n / d, r = n % d;
    __int128 ar = r < 0 ? -r : r, ad = d < 0 ? -d : d;
    if (2 * ar >= ad) q += ((n < 0) == (d < 0)) ? 1 : -1;
    return q;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool all_digits(const std::string& s, size_t a, size_t b) {
    if (a >= b) return false;
    for (size_t k = a; k < b; ++k)
        if (!std::isdigit(static_cast<unsigned char>(s[k]))) return false;
    return true;
}

int days_in_month(int y, int m) {
    static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return m == 2 && leap ? 29 : d[m - 1];
}

// sign, digits, optional decimal mark + digits; round_extra: more than 2 decimals are rounded instead of refused
bool parse_decimal(const std::string& s, bool allow_comma, bool round_extra, int64_t& out) {
    size_t k = 0;
    bool neg = false;
    if (k < s.size() && (s[k] == '-' || s[k] == '+')) { neg = s[k] == '-'; ++k; }
    size_t int_start = k;
    while (k < s.size() && std::isdigit(static_cast<unsigned char>(s[k]))) ++k;
    size_t int_end = k;
    size_t frac_start = k, frac_end = k;
    if (k < s.size() && (s[k] == '.' || (allow_comma && s[k] == ','))) {
        frac_start = ++k;
        while (k < s.size() && std::isdigit(static_cast<unsigned char>(s[k]))) ++k;
        frac_end = k;
        if (frac_end == frac_start) return false;  // "5." is not a number
    }
    if (k != s.size() || int_end == int_start) return false;
    if (!round_extra && frac_end - frac_start > 2) return false;
    if (int_end - int_start > 18) return false;
    __int128 v = 0;
    for (size_t j = int_start; j < int_end; ++j) v = v * 10 + (s[j] - '0');
    v *= 100;
    // the first two decimals, then round on the third
    int64_t frac = 0;
    for (size_t j = 0; j < 2; ++j) frac = frac * 10 + (frac_start + j < frac_end ? s[frac_start + j] - '0' : 0);
    v += frac;
    if (frac_end - frac_start > 2 && s[frac_start + 2] >= '5') v += 1;
    if (neg) v = -v;
    if (v > kMax || v < kMin) return false;
    out = static_cast<int64_t>(v);
    return true;
}

}  // namespace

const char* type_name(Type t) {
    switch (t) {
        case Type::Number: return "Number";
        case Type::Time: return "Time";
        case Type::Date: return "Date";
        case Type::Bool: return "Bool";
        case Type::Text: return "Text";
        default: return "Invalid";
    }
}

Type type_from_name(const std::string& s) {
    if (s == "Number") return Type::Number;
    if (s == "Time") return Type::Time;
    if (s == "Date") return Type::Date;
    if (s == "Bool") return Type::Bool;
    if (s == "Text") return Type::Text;
    return Type::Invalid;
}

// Howard Hinnant's civil calendar algorithms
int64_t days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void civil_from_days(int64_t z, int& y, int& m, int& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    y = static_cast<int>(yoe + era * 400 + (m <= 2));
}

bool parse_number_literal(const std::string& s, int64_t& out) { return parse_decimal(s, false, false, out); }

bool parse_time_literal(const std::string& s, int64_t& out) {
    size_t colon = s.find(':');
    if (colon == std::string::npos || colon == 0 || colon > 2 || s.size() != colon + 3) return false;
    if (!all_digits(s, 0, colon) || !all_digits(s, colon + 1, s.size())) return false;
    int h = std::stoi(s.substr(0, colon)), mi = std::stoi(s.substr(colon + 1));
    if (h > 23 || mi > 59) return false;
    out = h * 60 + mi;
    return true;
}

bool parse_date_literal(const std::string& s, int64_t& out) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
    if (!all_digits(s, 0, 4) || !all_digits(s, 5, 7) || !all_digits(s, 8, 10)) return false;
    int y = std::stoi(s.substr(0, 4)), m = std::stoi(s.substr(5, 2)), d = std::stoi(s.substr(8, 2));
    if (y < kMinYear || m < 1 || m > 12 || d < 1 || d > days_in_month(y, m)) return false;
    out = days_from_civil(y, m, d);
    return true;
}

bool parse_value(Type t, const std::string& s, Value& out) {
    int64_t i = 0;
    switch (t) {
        case Type::Number: if (!parse_number_literal(s, i)) return false; out = Value::number(i); return true;
        case Type::Time: if (!parse_time_literal(s, i)) return false; out = Value::time(i); return true;
        case Type::Date: if (!parse_date_literal(s, i)) return false; out = Value::date(i); return true;
        case Type::Bool:
            if (s == "true") { out = Value::boolean(true); return true; }
            if (s == "false") { out = Value::boolean(false); return true; }
            return false;
        case Type::Text: out = Value::text(s); return true;
        default: return false;
    }
}

bool parse_value_loose(Type t, const std::string& raw, Value& out) {
    if (t == Type::Text) { out = Value::text(trim(raw)); return true; }
    std::string s = trim(raw);
    int64_t i = 0;
    switch (t) {
        case Type::Number:
            if (!parse_decimal(s, true, true, i)) return false;
            out = Value::number(i);
            return true;
        case Type::Time: {
            std::string c = s;
            size_t dot = c.find('.');
            if (c.find(':') == std::string::npos && dot != std::string::npos) c[dot] = ':';
            if (!parse_time_literal(c, i)) return false;
            out = Value::time(i);
            return true;
        }
        case Type::Date:
            if (!parse_date_literal(s, i)) return false;
            out = Value::date(i);
            return true;
        case Type::Bool: {
            std::string l = lower(s);
            if (l == "true" || l == "yes" || l == "1" || l == "evet") { out = Value::boolean(true); return true; }
            if (l == "false" || l == "no" || l == "0" || l == "hayır" || l == "hayir") { out = Value::boolean(false); return true; }
            return false;
        }
        default: return false;
    }
}

std::string format_value(const Value& v, const std::string& lang) {
    char buf[64];
    switch (v.type) {
        case Type::Number: {
            uint64_t a = v.i < 0 ? 0 - static_cast<uint64_t>(v.i) : static_cast<uint64_t>(v.i);
            snprintf(buf, sizeof buf, "%s%llu%c%02llu", v.i < 0 ? "-" : "", static_cast<unsigned long long>(a / 100),
                     lang == "tr" ? ',' : '.', static_cast<unsigned long long>(a % 100));
            return buf;
        }
        case Type::Time:
            snprintf(buf, sizeof buf, "%02d:%02d", static_cast<int>(v.i / 60), static_cast<int>(v.i % 60));
            return buf;
        case Type::Date: {
            int y, m, d;
            civil_from_days(v.i, y, m, d);
            snprintf(buf, sizeof buf, "%04d-%02d-%02d", y, m, d);
            return buf;
        }
        case Type::Bool: return v.b ? "true" : "false";
        case Type::Text: return v.s;
        default: return "";
    }
}

std::string literal_text(const Value& v) { return format_value(v, "en"); }

json value_to_json(const Value& v) {
    json j = {{"type", type_name(v.type)}};
    switch (v.type) {
        case Type::Number: case Type::Time: case Type::Date: j["value"] = v.i; break;
        case Type::Bool: j["value"] = v.b; break;
        case Type::Text: j["value"] = v.s; break;
        default: j["value"] = nullptr;
    }
    return j;
}

bool value_from_json(const json& j, Value& out) {
    if (!j.is_object() || !j.contains("type") || !j["type"].is_string() || !j.contains("value")) return false;
    Type t = type_from_name(j["type"].get<std::string>());
    const json& v = j["value"];
    switch (t) {
        case Type::Number: case Type::Time: case Type::Date: {
            if (!v.is_number_integer()) return false;
            int64_t i = v.get<int64_t>();
            if (t == Type::Time && (i < 0 || i > 1439)) return false;
            out = Value{t, i, false, {}};
            return true;
        }
        case Type::Bool: if (!v.is_boolean()) return false; out = Value::boolean(v.get<bool>()); return true;
        case Type::Text: if (!v.is_string()) return false; out = Value::text(v.get<std::string>()); return true;
        default: return false;
    }
}

int64_t num_add(int64_t a, int64_t b) {
    int64_t r;
    if (__builtin_add_overflow(a, b, &r)) throw ScriptError("Number too big");
    return r;
}

int64_t num_sub(int64_t a, int64_t b) {
    int64_t r;
    if (__builtin_sub_overflow(a, b, &r)) throw ScriptError("Number too big");
    return r;
}

int64_t num_mul(int64_t a, int64_t b) { return to_i64(div_round(static_cast<__int128>(a) * b, 100)); }

int64_t num_div(int64_t a, int64_t b) {
    if (b == 0) throw ScriptError("Division by zero");
    return to_i64(div_round(static_cast<__int128>(a) * 100, b));
}

int64_t num_mod(int64_t a, int64_t b) {
    if (b == 0) throw ScriptError("Division by zero");
    if (b == -1) return 0;
    return a % b;
}

int64_t num_round(int64_t a) { return to_i64(div_round(a, 100) * 100); }

}  // namespace pc2::script
