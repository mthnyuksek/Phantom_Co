"""Tests for visual scripts. Step A1: values and expressions through the --script-eval hook (no server needed).

Usage: python -X utf8 -I script_test.py <absolute path of phantom_co.exe>
"""
import json
import subprocess
import sys

failures = []


def check(name, cond, extra=""):
    print(("PASS " if cond else "FAIL ") + name + (f"  [{extra}]" if extra and not cond else ""))
    if not cond:
        failures.append(name)


def run_requests(exe, reqs):
    data = "".join(json.dumps(r) + "\n" for r in reqs)
    p = subprocess.run([exe, "--script-eval"], input=data.encode("utf-8"), capture_output=True, timeout=60)
    lines = p.stdout.decode("utf-8").splitlines()
    return [json.loads(x) for x in lines]


N = lambda v: {"type": "Number", "value": v}
T = lambda v: {"type": "Time", "value": v}
D = lambda v: {"type": "Date", "value": v}
B = lambda v: {"type": "Bool", "value": v}
S = lambda v: {"type": "Text", "value": v}

VARS = {
    "a": N("56.00"), "b": N("2.50"), "neg": N("-3.25"), "big": N("90000000000000000.00"),
    "t": T("15:30"), "late": T("23:50"), "d": D("2026-11-16"), "yes": B("true"), "name": S("Quill"),
    "claude_left_5h": N("64.00"),
}

# (expr, expected type, expected text) or (expr, None, stage of the failure)
CASES = [
    # numbers: fixed point, 2 decimals
    ("1.50 + 2.25", "Number", "3.75"),
    ("a / 100", "Number", "0.56"),
    ("10 / 3", "Number", "3.33"),
    ("20 / 3", "Number", "6.67"),
    ("-20 / 3", "Number", "-6.67"),
    ("0.05 * 0.5", "Number", "0.03"),     # 0.025 -> half away from zero
    ("-0.05 * 0.5", "Number", "-0.03"),
    ("a * b", "Number", "140.00"),
    ("7 % 3", "Number", "1.00"),
    ("7.5 % 2", "Number", "1.50"),
    ("neg", "Number", "-3.25"),
    ("-neg", "Number", "3.25"),
    ("2 + 3 * 4", "Number", "14.00"),
    ("(2 + 3) * 4", "Number", "20.00"),
    ("abs(neg)", "Number", "3.25"),
    ("round(2.50)", "Number", "3.00"),
    ("round(-2.50)", "Number", "-3.00"),
    ("round(2.49)", "Number", "2.00"),
    ("min(a, b)", "Number", "2.50"),
    ("max(a, b)", "Number", "56.00"),
    ("1 / 0", None, "eval"),
    ("5 % 0", None, "eval"),
    ("big * 1000", None, "eval"),
    ("big + big", None, "eval"),
    ("1.234", None, "parse"),
    ("5.", None, "parse"),
    ("12abc", None, "parse"),
    # time
    ("t + 45", "Time", "16:15"),
    ("late + 20", "Time", "00:10"),
    ("00:10 - 20", "Time", "23:50"),
    ("t - 09:00", "Number", "390.00"),
    ("t < 16:00", "Bool", "true"),
    ("t >= 16:00", "Bool", "false"),
    ("min(t, 12:00)", "Time", "12:00"),
    ("t + 0.5", None, "eval"),
    ("24:00", None, "parse"),
    ("9:75", None, "parse"),
    ("t < 16", None, "type"),
    ("t + t", None, "type"),
    # dates
    ("d + 30", "Date", "2026-12-16"),
    ("d - 2026-11-01", "Number", "15.00"),
    ("2024-02-28 + 1", "Date", "2024-02-29"),
    ("2023-02-28 + 1", "Date", "2023-03-01"),
    ("d > 2026-11-06", "Bool", "true"),
    ("2026-02-30", None, "parse"),
    ("9999-12-31 + 1", None, "eval"),
    ("d < 261116", None, "type"),
    # logic
    ("claude_left_5h > 50 and t < 16:00", "Bool", "true"),
    ("claude_left_5h > 70 or not yes", "Bool", "false"),
    ("!(a == 56) || b != 2.5", "Bool", "false"),
    ("yes == true", "Bool", "true"),
    ("false and 1 / 0 > 1", "Bool", "false"),   # short circuit
    ("yes < false", None, "type"),
    ("1 < 2 < 3", None, "parse"),
    ("a = 5", None, "parse"),
    ("yes and 1", None, "type"),
    # text
    ('name + " here"', "Text", "Quill here"),
    ('name == "Quill"', "Bool", "true"),
    ('"say \\"hi\\""', "Text", 'say "hi"'),
    ('name + 1', None, "type"),
    ('"open', None, "parse"),
    # names and calls
    ("missing + 1", None, "type"),
    ("foo(1)", None, "type"),
    ("abs(1, 2)", None, "type"),
    ("min(a, t)", None, "type"),
    ("", None, "parse"),
    ("(1 + 2", None, "parse"),
    ("1 +", None, "parse"),
    ("a $ b", None, "parse"),
]

# loose values (from agents and input fields)
LOOSE = [
    ("Number", " 56,5 ", "56.50"),
    ("Number", "1.005", "1.01"),
    ("Number", "-0.004", "0.00"),
    ("Number", "1.999", "2.00"),
    ("Number", "abc", None),
    ("Time", "16.00", "16:00"),
    ("Time", "7:05", "07:05"),
    ("Time", "25:00", None),
    ("Date", "2026-11-16", "2026-11-16"),
    ("Date", "261116", None),
    ("Bool", "Yes", "true"),
    ("Bool", "hayır", "false"),
    ("Bool", "maybe", None),
    ("Text", "  hi  ", "hi"),
]


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    exe = sys.argv[1]
    reqs = [{"expr": e, "vars": VARS} for e, _, _ in CASES] + [{"loose": {"type": t, "text": x}} for t, x, _ in LOOSE]
    reqs.append({"expr": "1", "vars": {"x": {"type": "Number", "value": "1.234"}}})  # bad variable value
    reqs.append({"expr": "a", "vars": {"a": N("1234.5")}})
    answers = run_requests(exe, reqs)
    check("one answer per request", len(answers) == len(reqs), f"{len(answers)} of {len(reqs)}")
    if len(answers) != len(reqs):
        sys.exit(1)

    for (expr, typ, want), got in zip(CASES, answers):
        if typ is None:
            check(f"fails at {want}: {expr!r}", not got["ok"] and got.get("stage") == want, json.dumps(got))
        else:
            check(f"{expr!r} = {want}", got["ok"] and got["type"] == typ and got["text"] == want, json.dumps(got))
    for (typ, text, want), got in zip(LOOSE, answers[len(CASES):]):
        if want is None:
            check(f"loose {typ} refuses {text!r}", not got["ok"], json.dumps(got))
        else:
            check(f"loose {typ} {text!r} = {want}", got["ok"] and got["text"] == want, json.dumps(got))

    bad_var, num = answers[-2], answers[-1]
    check("bad variable value refused", not bad_var["ok"] and bad_var["stage"] == "vars", json.dumps(bad_var))
    check("number kept as hundredths", num["ok"] and num["value"] == 123450, json.dumps(num))
    check("Turkish decimal comma", num["ok"] and num["text_tr"] == "1234,50", json.dumps(num))

    first_err = next(a for (e, _, _), a in zip(CASES, answers) if e == "a $ b")
    check("error position points at the bad character", first_err.get("pos") == 2, json.dumps(first_err))

    print(f"\n{len(failures)} failed" if failures else "\nALL PASSED")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
