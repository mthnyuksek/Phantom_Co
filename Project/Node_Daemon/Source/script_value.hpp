#pragma once
// Values of visual scripts: Number (fixed point, 2 decimals), Time, Date, Bool, Text.
// See Development/Docs/visual_scripts.md.
#include <cstdint>
#include <stdexcept>
#include <string>

#include "util.hpp"

namespace pc2::script {

enum class Type { Number, Time, Date, Bool, Text, Invalid };

const char* type_name(Type t);           // "Number", "Time", ...
Type type_from_name(const std::string& s);  // Invalid for an unknown name

// Number: i = hundredths (56.00 -> 5600). Time: i = minutes since midnight (0-1439). Date: i = days since 1970-01-01.
struct Value {
    Type type = Type::Invalid;
    int64_t i = 0;
    bool b = false;
    std::string s;

    static Value number(int64_t hundredths) { return {Type::Number, hundredths, false, {}}; }
    static Value time(int64_t minutes) { return {Type::Time, minutes, false, {}}; }
    static Value date(int64_t days) { return {Type::Date, days, false, {}}; }
    static Value boolean(bool v) { return {Type::Bool, 0, v, {}}; }
    static Value text(std::string v) { return {Type::Text, 0, false, std::move(v)}; }
};

// A failure while evaluating (overflow, division by zero, wrong type); pos = byte offset in the expression or npos.
struct ScriptError : std::runtime_error {
    size_t pos;
    explicit ScriptError(const std::string& msg, size_t p = std::string::npos) : std::runtime_error(msg), pos(p) {}
};

constexpr int kMinYear = 1, kMaxYear = 9999;

int64_t days_from_civil(int y, int m, int d);
void civil_from_days(int64_t days, int& y, int& m, int& d);

// Literals as written in expressions and script files: "56.00", "-3", "16:00", "2026-11-16", "true".
// Strict: at most 2 decimals, '.' only. Return false on a bad text.
bool parse_number_literal(const std::string& s, int64_t& out);
bool parse_time_literal(const std::string& s, int64_t& out);
bool parse_date_literal(const std::string& s, int64_t& out);
// A value of the given type from a literal text (Text is taken as it is, without quotes).
bool parse_value(Type t, const std::string& s, Value& out);
// Loose parse for values that come from agents or input fields: trims, accepts ',' or '.' as the decimal mark,
// rounds more than 2 decimals half away from zero, "16.00" is accepted as a Time, "yes"/"no" as Bool.
bool parse_value_loose(Type t, const std::string& s, Value& out);

// How the UI shows a value: lang "tr" uses a decimal comma for numbers.
std::string format_value(const Value& v, const std::string& lang = "en");
// Literal text that parse_value reads back (Number always with '.', Text unquoted).
std::string literal_text(const Value& v);

// JSON of a run state: {"type": "Number", "value": 5600} (Number/Time/Date as integers, Bool, Text as strings).
json value_to_json(const Value& v);
bool value_from_json(const json& j, Value& out);

// Arithmetic with overflow checks (throw ScriptError).
int64_t num_add(int64_t a, int64_t b);
int64_t num_sub(int64_t a, int64_t b);
int64_t num_mul(int64_t a, int64_t b);  // hundredths * hundredths / 100, rounded half away from zero
int64_t num_div(int64_t a, int64_t b);  // a * 100 / b, rounded half away from zero
int64_t num_mod(int64_t a, int64_t b);
int64_t num_round(int64_t a);           // to a whole number

}  // namespace pc2::script
