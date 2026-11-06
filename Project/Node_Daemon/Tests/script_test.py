"""Tests for visual scripts: values and expressions through the --script-eval hook, then the script API
on a temporary Vault (port 7792, demo ghosts only, costs nothing).

Usage: python -X utf8 -I script_test.py <absolute path of phantom_co.exe>
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

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


# ---------------------------------------------------------------- server part
PORT = 7792
BASE = f"http://127.0.0.1:{PORT}"


class Client:
    """One signed-in browser: keeps its session cookie."""

    def __init__(self):
        self.cookie = ""

    def call(self, method, path, body=None):
        data = json.dumps(body).encode() if body is not None else None
        h = {"X-PC2": "1"}
        if body is not None:
            h["Content-Type"] = "application/json"
        if self.cookie:
            h["Cookie"] = self.cookie
        req = urllib.request.Request(BASE + path, data=data, method=method, headers=h)
        try:
            with urllib.request.urlopen(req, timeout=15) as r:
                raw = r.read()
                sc = r.headers.get("Set-Cookie")
                if sc and "pc2_session_" in sc:
                    tok = sc.split(f"pc2_session_{PORT}=")[1].split(";")[0]
                    self.cookie = f"pc2_session_{PORT}={tok}" if tok else ""
                return r.status, (json.loads(raw) if raw[:1] in (b"{", b"[") else raw)
        except urllib.error.HTTPError as e:
            raw = e.read()
            try:
                return e.code, json.loads(raw)
            except Exception:
                return e.code, raw

    def get(self, p): return self.call("GET", p)
    def post(self, p, b=None): return self.call("POST", p, b if b is not None else {})
    def put(self, p, b): return self.call("PUT", p, b)
    def delete(self, p): return self.call("DELETE", p)


def good_script():
    return {
        "name": "Report flow",
        "vars": [{"name": "deadline", "type": "Date", "init": "2026-11-16"}, {"name": "score", "type": "Number", "init": "0"}],
        "nodes": [
            {"id": "s", "type": "start", "x": 0, "y": 0},
            {"id": "p1", "type": "prompt", "props": {"agent_id": "quill", "text": "Write the report, due {deadline}.", "target": "score"}},
            {"id": "if1", "type": "if", "props": {"expr": "claude_left_5h > 50 and score >= 7.5"}},
            {"id": "p2", "type": "prompt", "props": {"agent_id": "bolt", "project_id": "moon_base", "text": "Build it."}},
            {"id": "w", "type": "delay", "props": {"mode": "for", "expr": "30"}},
            {"id": "p3", "type": "prompt", "props": {"agent_id": "quill", "text": "Present it."}},
            {"id": "set1", "type": "set", "props": {"var": "score", "expr": "score + 1"}},
            {"id": "e", "type": "end"},
        ],
        "edges": [
            {"from": "s", "to": "p1"}, {"from": "p1", "to": "if1"},
            {"from": "if1", "port": "true", "to": "p2"}, {"from": "if1", "port": "false", "to": "set1"},
            {"from": "p2", "to": "w"}, {"from": "w", "to": "p3"}, {"from": "p3", "to": "e"}, {"from": "set1", "to": "e"},
        ],
    }


def errors_of(check_result):
    return [e["message"] for e in check_result["errors"]]


def chain(name, nodes, edges=None, vars=(), **extra):
    """A script; without edges the blocks are joined one after another."""
    if edges is None:
        edges = [{"from": a["id"], "to": b["id"]} for a, b in zip(nodes, nodes[1:])]
    return dict({"name": name, "vars": list(vars), "nodes": nodes, "edges": edges}, **extra)


def P(id, agent, text, **props):
    return {"id": id, "type": "prompt", "props": dict({"agent_id": agent, "text": text}, **props)}


START, END = {"id": "s", "type": "start"}, {"id": "e", "type": "end"}


def make_and_run(client, script):
    st, s = client.post("/api/scripts", script)
    if st != 201:
        return st, s
    return client.post(f"/api/scripts/{s['id']}/run")


def wait_run(client, run_id, until=("done", "failed", "stopped"), timeout=60, cond=None):
    t0 = time.time()
    r = None
    while time.time() - t0 < timeout:
        st, r = client.get("/api/script_runs/" + run_id)
        if st == 200 and (cond(r) if cond else r["status"] in until):
            return r
        time.sleep(0.15)
    return r


def node_state(r, nid):
    return r["nodes"][nid]["state"] if r else None


def event_index(r, kind, node):
    for i, e in enumerate(r["events"]):
        if e.get("kind") == kind and e.get("node") == node:
            return i
    return -1


def login(name, password):
    c = Client()
    c.post("/api/auth/login", {"name": name, "password": password})
    return c


def run_tests(exe, vault, data, procs, owner, ece, vera):
    # 1. value from a ghost, If, skip, join, {var} in a prompt
    sc = chain("Rate and build", [
        START,
        P("rate", "quill", "<value>7,5</value> rate it", target="score"),
        {"id": "ok", "type": "if", "props": {"expr": "score >= 7.5"}},
        P("build", "bolt", "Build {score} for {who}"),
        {"id": "zero", "type": "set", "props": {"var": "score", "expr": "0"}},
        END,
    ], edges=[{"from": "s", "to": "rate"}, {"from": "rate", "to": "ok"},
              {"from": "ok", "port": "true", "to": "build"}, {"from": "ok", "port": "false", "to": "zero"},
              {"from": "build", "to": "e"}, {"from": "zero", "to": "e"}],
        vars=[{"name": "score", "type": "Number", "init": "0"}, {"name": "who", "type": "Text", "init": "Meto"}])
    st, r = make_and_run(ece, sc)
    check("editor starts a run", st == 201 and r.get("status") in ("running", "done"), r)
    r = wait_run(ece, r["id"])
    check("run finishes", r and r["status"] == "done", r and (r["status"], r.get("error")))
    check("value taken from the ghost's answer", r and r["vars"]["score"] == {"type": "Number", "value": 750}, r and r["vars"])
    check("true branch ran, false branch skipped", r and node_state(r, "build") == "done" and node_state(r, "zero") == "skipped")
    check("join after an If still runs", r and node_state(r, "e") == "done")
    check("{var} filled into the prompt", r and "Build 7.50 for Meto" in r["nodes"]["build"].get("output", ""), r and r["nodes"]["build"])
    check("run state file in the Vault", r and os.path.exists(os.path.join(vault, "Script_Runs", r["id"] + ".json")))
    conv = r["nodes"]["rate"].get("conv", "") if r else ""
    st, cv = vera.get("/api/chats/" + conv)
    check("block conversation visible to the team", st == 200, cv)
    st, chats = vera.get("/api/chats")
    check("block conversations stay out of the chat list", st == 200 and all(c.get("kind") != "script" for c in chats["chats"]))
    st, runs = vera.get("/api/script_runs?script=rate_and_build")
    check("runs listed per script", st == 200 and len(runs["runs"]) == 1 and runs["runs"][0]["status"] == "done", runs)

    # 2. parallel branches, join waits for all, delay
    sc = chain("Fan out", [START, P("a", "quill", "A demo:write"), P("b", "bolt", "B"),
                           {"id": "join", "type": "set", "props": {"var": "n", "expr": "n + 1"}},
                           {"id": "wait", "type": "delay", "props": {"mode": "for", "expr": "20"}}, END],
               edges=[{"from": "s", "to": "a"}, {"from": "s", "to": "b"}, {"from": "a", "to": "join"}, {"from": "b", "to": "join"},
                      {"from": "join", "to": "wait"}, {"from": "wait", "to": "e"}],
               vars=[{"name": "n", "type": "Number"}])
    t0 = time.time()
    st, r = make_and_run(ece, sc)
    r1 = wait_run(ece, r["id"], cond=lambda x: node_state(x, "wait") == "running")
    check("both branches ran before the join", r1 and min(event_index(r1, "node_done", "a"), event_index(r1, "node_done", "b")) >= 0
          and max(event_index(r1, "node_done", "a"), event_index(r1, "node_done", "b")) < event_index(r1, "node_started", "join"), r1 and r1["events"])
    check("both branches started together", r1 and event_index(r1, "node_started", "b") < event_index(r1, "node_done", "a"), r1 and r1["events"])
    check("delay waits", r1 and r1["status"] == "running")
    r = wait_run(ece, r["id"])
    check("run after the delay finishes", r and r["status"] == "done" and r["vars"]["n"]["value"] == 100, r and (r["status"], r["vars"]))
    check("delay took its time (20 test minutes = 2 s)", time.time() - t0 >= 2.0)

    # 2b. prompt -> delay -> prompt (the shape a person tried first)
    st, r = make_and_run(ece, chain("Prompt delay prompt", [START, P("a", "quill", "first"),
                                                            {"id": "w", "type": "delay", "props": {"mode": "for", "expr": "10"}},
                                                            P("b", "bolt", "second"), END]))
    r = wait_run(ece, r["id"])
    check("prompt after a delay after a prompt runs", r and r["status"] == "done" and node_state(r, "b") == "done"
          and "second" in r["nodes"]["b"].get("output", ""), r and (r["status"], r["nodes"]))

    # 3. no usable value: asked once more, then the run fails
    st, r = make_and_run(ece, chain("No value", [START, P("p", "quill", "nothing here", target="when"), END],
                                    vars=[{"name": "when", "type": "Date"}]))
    r = wait_run(ece, r["id"])
    check("missing value fails the run after one retry", r and r["status"] == "failed" and node_state(r, "p") == "failed"
          and "usable value" in r["nodes"]["p"].get("error", "") and len(r["nodes"]["p"].get("runs", [])) == 2, r and r["nodes"]["p"])
    check("retry noted in the events", r and any("asking once more" in e.get("text", "") for e in r["events"]))
    check("later blocks never start after a failure", r and node_state(r, "e") == "waiting")

    # 4. a ghost asks a helper: the block waits for the report
    st, r = make_and_run(ece, chain("Helper", [START, P("p", "quill", "demo:send:bolt"), END]))
    r = wait_run(ece, r["id"])
    check("block waits for the helper's report", r and r["status"] == "done" and r["nodes"]["p"].get("output", "").startswith("Thanks, I got the report"),
          r and r["nodes"]["p"].get("output"))

    # 5. stop, rights, one run at a time, delete while running
    st, s = ece.post("/api/scripts", chain("Slow", [START, P("p", "quill", "demo:slow"), END]))
    st, r = ece.post("/api/scripts/slow/run")
    check("slow run started", st == 201, r)
    check("a script runs once at a time", ece.post("/api/scripts/slow/run")[0] == 409)
    check("viewer cannot run", vera.post("/api/scripts/slow/run")[0] == 403)
    check("running script cannot be deleted", ece.delete("/api/scripts/slow")[0] == 409)
    st, other = owner.post("/api/scripts", chain("Owner slow", [START, P("p", "quill", "demo:slow"), END]))
    st, orun = owner.post("/api/scripts/owner_slow/run")
    check("someone else's run cannot be stopped", ece.post(f"/api/script_runs/{orun['id']}/stop")[0] == 403)
    check("owner stops any run", owner.post(f"/api/script_runs/{r['id']}/stop")[0] == 200)
    owner.post(f"/api/script_runs/{orun['id']}/stop")
    r = wait_run(ece, r["id"])
    check("stopped run", r and r["status"] == "stopped" and node_state(r, "p") == "stopped", r and (r["status"], r["nodes"]))
    check("stop on an ended run refused", ece.post(f"/api/script_runs/{r['id']}/stop")[0] == 409)
    time.sleep(0.5)
    st, cv = ece.get("/api/chats/" + r["nodes"]["p"]["conv"])
    check("the ghost was stopped too", st == 200 and not cv.get("busy"), cv)

    # 6. pause and step
    st, r = make_and_run(ece, chain("Steps", [START, P("p", "quill", "hi"), {"id": "a", "type": "set", "props": {"var": "x", "expr": "x + 1"}},
                                               {"id": "b", "type": "set", "props": {"var": "x", "expr": "x * 10"}}, END],
                                    vars=[{"name": "x", "type": "Number"}]))
    rid = r["id"]
    check("pause", ece.post(f"/api/script_runs/{rid}/pause")[0] == 200)
    check("pause twice refused", ece.post(f"/api/script_runs/{rid}/pause")[0] == 409)
    r = wait_run(ece, rid, cond=lambda x: node_state(x, "a") == "ready")
    check("paused run keeps the next block waiting", r and r["status"] == "paused" and node_state(r, "p") == "done", r and r["nodes"])
    st, r = ece.post(f"/api/script_runs/{rid}/step")
    check("step starts exactly one block", st == 200 and node_state(r, "a") == "done" and node_state(r, "b") == "ready" and r["vars"]["x"]["value"] == 100, r)
    st, r = ece.post(f"/api/script_runs/{rid}/resume")
    check("resume runs the rest", st == 200 and r["status"] == "done" and r["vars"]["x"]["value"] == 1000, r)
    check("step on an ended run refused", ece.post(f"/api/script_runs/{rid}/step")[0] == 409)

    # 7. runs refused or failing cleanly
    st, r = ece.post("/api/scripts/broken/run")
    check("a script with problems does not run", st == 400 and r.get("check", {}).get("errors"), r)
    st, r = make_and_run(ece, chain("Divide", [START, {"id": "d", "type": "set", "props": {"var": "x", "expr": "x / 0"}}, END],
                                    vars=[{"name": "x", "type": "Number", "init": "1"}]))
    r = wait_run(ece, r["id"])
    check("division by zero fails the run", r and r["status"] == "failed" and "Division by zero" in r.get("error", ""), r and r.get("error"))
    st, r = make_and_run(ece, chain("Usage", [START, {"id": "u", "type": "if", "props": {"expr": "claude_left_5h > 50 and claude_left_5h < 64.01"}},
                                               {"id": "t", "type": "set", "props": {"var": "tok", "expr": "tokens_today"}}, END],
                                    edges=[{"from": "s", "to": "u"}, {"from": "u", "port": "true", "to": "t"}, {"from": "t", "to": "e"}],
                                    vars=[{"name": "tok", "type": "Number"}]))
    r = wait_run(ece, r["id"])
    check("claude_left_5h read from the last report", r and r["status"] == "done" and r["nodes"]["u"].get("output") == "true", r and (r["status"], r.get("error")))
    check("fresh usage is not read again", r and not any("usage limit" in e.get("text", "") for e in r["events"]), r and r["events"])
    check("tokens_today counts today's runs", r and r["vars"]["tok"]["value"] > 0, r and r["vars"])
    st, r = make_and_run(ece, chain("Old usage", [START, {"id": "u", "type": "if", "props": {"expr": "codex_left > 98.5"}}, END],
                                    edges=[{"from": "s", "to": "u"}, {"from": "u", "port": "true", "to": "e"}]))
    r = wait_run(ece, r["id"])
    check("old usage: a fresh reading is tried first", r and any("usage limit" in e.get("text", "") for e in r["events"]), r and r["events"])
    check("old usage still used when no fresh reading comes", r and r["status"] == "done" and node_state(r, "e") == "done", r and (r["status"], r.get("error")))
    st, r = make_and_run(ece, chain("Weekly", [START, {"id": "u", "type": "if", "props": {"expr": "claude_left_week > 10"}}, END],
                                    edges=[{"from": "s", "to": "u"}, {"from": "u", "port": "true", "to": "e"}]))
    r = wait_run(ece, r["id"])
    check("usage never reported fails cleanly", r and r["status"] == "failed" and "not known" in r.get("error", ""), r and r.get("error"))
    st, u = ece.get("/api/usage")
    check("usage visible to editors", st == 200 and u["usage"]["claude"]["left_5h"] == 64.0 and u["usage"]["claude"]["refreshing"] is False, u)
    check("viewer cannot see usage", vera.get("/api/usage")[0] == 403)
    check("only the owner asks for a fresh reading", ece.post("/api/usage/refresh", {"brain": "claude"})[0] == 403
          and owner.post("/api/usage/refresh", {"brain": "claude"})[0] == 200 and owner.post("/api/usage/refresh", {"brain": "gemini"})[0] == 400)
    st, s = ece.post("/api/scripts", chain("Agent draft", [START, END], draft=True))
    check("draft saved as draft", s.get("draft") is True)
    ece.post("/api/scripts/agent_draft/run")
    check("running a draft makes it a normal script", ece.get("/api/scripts/agent_draft")[1].get("draft") is False)

    # 8. a run goes on after the program closed
    st, r = make_and_run(ece, chain("Survivor", [START, {"id": "w", "type": "delay", "props": {"mode": "for", "expr": "30"}},
                                                  P("p", "quill", "demo:slow after"), END]))
    rid = r["id"]
    time.sleep(1)
    stop_server(procs[-1], hard=True)
    procs.append(start_server(exe, vault, data))
    ece2 = login("Ece", "ece password 1")
    r = wait_run(ece2, rid, cond=lambda x: node_state(x, "p") == "running", timeout=20)
    check("delay goes on after a restart", r and r["status"] == "running" and node_state(r, "w") == "done", r and (r["status"], r["nodes"]))
    time.sleep(1)
    stop_server(procs[-1], hard=True)
    procs.append(start_server(exe, vault, data))
    ece3 = login("Ece", "ece password 1")
    r = wait_run(ece3, rid, timeout=60)
    check("interrupted prompt starts again after a restart", r and r["status"] == "done" and r["nodes"]["p"].get("restarted") is True,
          r and (r["status"], r["nodes"]["p"]))
    check("restart noted in the events", r and any("Interrupted by a restart" in e.get("text", "") for e in r["events"]))


def start_server(exe, vault, data):
    # a script minute lasts 0.1 s; no real CLI is started to read usage limits
    env = dict(os.environ, PHANTOM_SCRIPT_MINUTE_MS="100", PHANTOM_USAGE_PROBE="off")
    proc = subprocess.Popen([exe, "--vault", vault, "--data", data, "--port", str(PORT), "--no-window"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
    for _ in range(60):
        try:
            Client().get("/api/auth/status")
            break
        except Exception:
            time.sleep(0.2)
    return proc


def stop_server(proc, hard=False):
    if hard:
        proc.kill()  # like a crash or a power cut
    else:
        proc.terminate()
    try:
        proc.wait(5)
    except Exception:
        proc.kill()


def server_tests(exe):
    tmp = tempfile.mkdtemp(prefix="pc2_script_")
    vault, data = os.path.join(tmp, "Vault"), os.path.join(tmp, "Data")
    os.makedirs(data, exist_ok=True)
    now = int(time.time())
    with open(os.path.join(data, "usage.json"), "w", encoding="utf-8") as f:  # as if a Claude run had just reported it
        json.dump({"claude": {"provider": "claude", "at": now, "left_5h": 64.0, "resets_5h": now + 3600},
                   "codex": {"provider": "codex", "at": now - 7200, "left": 99.0, "window_minutes": 43200}}, f)
    procs = [start_server(exe, vault, data)]
    try:
        owner, ece, vera = Client(), Client(), Client()
        owner.post("/api/auth/setup", {"name": "Meto", "password": "correct horse 1"})
        tok_e = owner.post("/api/users", {"name": "Ece", "role": "editor"})[1]["invite_token"]
        tok_v = owner.post("/api/users", {"name": "Vera", "role": "viewer"})[1]["invite_token"]
        ece.post("/api/auth/join", {"token": tok_e, "password": "ece password 1"})
        vera.post("/api/auth/join", {"token": tok_v, "password": "vera password 1"})
        owner.post("/api/agents", {"name": "Quill", "provider": "demo"})
        owner.post("/api/agents", {"name": "Bolt", "provider": "demo"})
        owner.post("/api/projects", {"name": "Moon Base", "summary": "x"})
        check("vault has Scripts and Script_Runs", os.path.isdir(os.path.join(vault, "Scripts")) and os.path.isdir(os.path.join(vault, "Script_Runs")))

        # create, read, list
        st, s = ece.post("/api/scripts", good_script())
        check("editor creates a script", st == 201 and s.get("id") == "report_flow", s)
        check("good script has no errors", st == 201 and s["check"]["errors"] == [], s.get("check"))
        check("prompt count", st == 201 and s["check"]["prompt_count"] == 3, s.get("check"))
        check("script file is JSON in the Vault", os.path.exists(os.path.join(vault, "Scripts", "report_flow.json")))
        check("maker recorded", s.get("created_by") == "ece" and s.get("draft") is False, s)
        st, lst = vera.get("/api/scripts")
        check("viewer lists scripts", st == 200 and [x["id"] for x in lst["scripts"]] == ["report_flow"], lst)
        check("list has counts", st == 200 and lst["scripts"][0]["prompt_count"] == 3 and lst["scripts"][0]["error_count"] == 0, lst)
        st, one = vera.get("/api/scripts/report_flow")
        check("viewer reads a script", st == 200 and len(one["nodes"]) == 8 and "check" in one)
        check("unknown script 404", vera.get("/api/scripts/nope")[0] == 404)

        # rights
        check("viewer cannot create", vera.post("/api/scripts", good_script())[0] == 403)
        st, s2 = owner.post("/api/scripts", dict(good_script(), name="Owner flow"))
        check("owner creates", st == 201 and s2["id"] == "owner_flow")
        check("editor cannot change another's script", ece.put("/api/scripts/owner_flow", {"name": "Mine"})[0] == 403)
        check("editor cannot delete another's script", ece.delete("/api/scripts/owner_flow")[0] == 403)
        st, s3 = owner.put("/api/scripts/report_flow", {"description": "Weekly"})
        check("owner changes any script, other fields kept", st == 200 and s3["description"] == "Weekly" and len(s3["nodes"]) == 8 and s3["created_by"] == "ece", s3)
        check("owner deletes", owner.delete("/api/scripts/owner_flow")[0] == 200 and not os.path.exists(os.path.join(vault, "Scripts", "owner_flow.json")))

        # structure refused outright
        def refused(name, body):
            st, r = ece.post("/api/scripts/validate", body)
            check("refused: " + name, st == 400, r)
        refused("no name", dict(good_script(), name=""))
        refused("unknown block type", dict(good_script(), nodes=[{"id": "x", "type": "loop"}]))
        refused("duplicate block id", dict(good_script(), nodes=[{"id": "x", "type": "start"}, {"id": "x", "type": "end"}]))
        refused("bad block id", dict(good_script(), nodes=[{"id": "a b", "type": "start"}]))
        refused("too many blocks", dict(good_script(), nodes=[{"id": f"n{i}", "type": "end"} for i in range(201)]))

        # problems listed, saved anyway
        def problems(name, mutate, want):
            sc = good_script()
            mutate(sc)
            st, r = ece.post("/api/scripts/validate", sc)
            msgs = errors_of(r) if st == 200 else []
            check("listed: " + name, st == 200 and any(want in m for m in msgs), msgs or r)
            return r

        problems("unknown ghost", lambda s: s["nodes"][1]["props"].update(agent_id="ghosty"), "Unknown ghost")
        problems("unknown project", lambda s: s["nodes"][3]["props"].update(project_id="mars"), "Unknown project")
        problems("empty prompt", lambda s: s["nodes"][1]["props"].update(text=""), "prompt is empty")
        problems("unknown placeholder", lambda s: s["nodes"][1]["props"].update(text="Use {nothing}"), "{nothing}")
        problems("condition not Bool", lambda s: s["nodes"][2]["props"].update(expr="score + 1"), "must give a Bool")
        r = problems("bad expression", lambda s: s["nodes"][2]["props"].update(expr="score >> 1"), "Unexpected")
        bad = [e for e in r["errors"] if e.get("node") == "if1"]
        check("expression error has node, field and position", bad and bad[0].get("field") == "expr" and bad[0].get("pos") == 7, bad)
        problems("set type mismatch", lambda s: s["nodes"][6]["props"].update(expr="deadline"), "must give a Number")
        problems("set built-in", lambda s: s["nodes"][6]["props"].update(var="today"), "built in")
        problems("delay until needs a Time", lambda s: s["nodes"][4]["props"].update(mode="until", expr="30"), "must give a Time")
        problems("bad start value", lambda s: s["vars"][0].update(init="261116"), "is not a Date")
        problems("reserved variable name", lambda s: s["vars"].append({"name": "and", "type": "Bool"}), "reserved")
        problems("built-in variable name", lambda s: s["vars"].append({"name": "now_time", "type": "Time"}), "built-in")
        problems("duplicate variable", lambda s: s["vars"].append({"name": "score", "type": "Number"}), "Two variables")
        problems("unknown type", lambda s: s["vars"].append({"name": "x", "type": "Float"}), "Unknown type")
        problems("no start", lambda s: s["nodes"].pop(0), "needs a Start")
        problems("two starts", lambda s: s["nodes"].append({"id": "s2", "type": "start"}), "only one Start")
        problems("loop", lambda s: s["edges"].append({"from": "p3", "to": "p1"}), "loop")
        problems("not connected", lambda s: s["nodes"].append({"id": "lonely", "type": "end"}), "not connected")
        problems("arrow into start", lambda s: s["edges"].append({"from": "p1", "to": "s"}), "Start block")
        problems("missing block", lambda s: s["edges"].append({"from": "p1", "to": "zzz"}), "missing block")
        problems("if needs true/false", lambda s: s["edges"].append({"from": "if1", "to": "p3"}), "no output")
        problems("end has no output", lambda s: s["edges"].append({"from": "e", "to": "p3"}), "no output")
        problems("same arrow twice", lambda s: s["edges"].append({"from": "s", "to": "p1"}), "twice")
        problems("self arrow", lambda s: s["edges"].append({"from": "p1", "to": "p1"}), "itself")

        sc = good_script()
        sc["edges"].append({"from": "p3", "to": "p1"})
        st, saved = ece.post("/api/scripts", dict(sc, name="Broken"))
        check("a script with problems is still saved", st == 201 and len(saved["check"]["errors"]) > 0, saved)
        st, lst = ece.get("/api/scripts")
        broken = [x for x in lst["scripts"] if x["id"] == "broken"]
        check("list counts the problems", broken and broken[0]["error_count"] > 0, lst)
        st, cleaned = ece.post("/api/scripts", {"name": "Clean me", "extra": 1, "nodes": [{"id": "s", "type": "start", "x": 1e12, "y": "a", "props": {"evil": 1}}]})
        check("unknown fields dropped, coordinates clamped", st == 201 and "extra" not in cleaned and cleaned["nodes"][0]["x"] == 100000
              and cleaned["nodes"][0]["y"] == 0 and cleaned["nodes"][0]["props"] == {}, cleaned)

        run_tests(exe, vault, data, procs, owner, ece, vera)
    finally:
        stop_server(procs[-1])
        shutil.rmtree(tmp, ignore_errors=True)


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

    server_tests(exe)

    print(f"\n{len(failures)} failed" if failures else "\nALL PASSED")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
