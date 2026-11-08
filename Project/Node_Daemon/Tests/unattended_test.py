"""Tests for what keeps Phantom Co. safe while nobody watches (v1.5): budgets and the Overview numbers, notifications,
Prompt retries and the "failed" arrow, the Ask block, global search, Vault backups, chat export, ghost memory.
Demo ghosts only, temporary Vault and data folder, port 7797, costs nothing.

Usage: python -X utf8 -I unattended_test.py <absolute path of phantom_co.exe>
"""
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile

PORT = 7797
BASE = f"http://127.0.0.1:{PORT}"
failures = []


def check(name, cond, extra=""):
    print(("PASS " if cond else "FAIL ") + name + (f"  [{extra}]" if extra and not cond else ""))
    if not cond:
        failures.append(name)


class Client:
    def __init__(self):
        self.cookie = ""

    def raw(self, method, path, body=None):
        data = json.dumps(body).encode() if body is not None else None
        h = {"X-PC2": "1", "Content-Type": "application/json"}
        if self.cookie:
            h["Cookie"] = self.cookie
        req = urllib.request.Request(BASE + path, data=data, method=method, headers=h)
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                sc = r.headers.get("Set-Cookie")
                if sc and "pc2_session_" in sc:
                    self.cookie = f"pc2_session_{PORT}=" + sc.split(f"pc2_session_{PORT}=")[1].split(";")[0]
                return r.status, r.read(), dict(r.headers)
        except urllib.error.HTTPError as e:
            return e.code, e.read(), dict(e.headers)

    def call(self, method, path, body=None):
        st, data, _ = self.raw(method, path, body)
        try:
            return st, json.loads(data or b"{}")
        except ValueError:
            return st, {}

    def get(self, p):
        return self.call("GET", p)

    def post(self, p, b=None):
        return self.call("POST", p, b if b is not None else {})

    def put(self, p, b):
        return self.call("PUT", p, b)

    def delete(self, p):
        return self.call("DELETE", p)


def chat(c, agent, text, conv=None):
    if conv is None:
        conv = c.post("/api/chats", {"agent_id": agent})[1]["meta"]["id"]
    st, r = c.post(f"/api/chats/{conv}/send", {"text": text})
    return conv, st, r


def wait_idle(c, conv, timeout=30):
    t0 = time.time()
    while time.time() - t0 < timeout:
        st, r = c.get("/api/chats/" + conv)
        if st == 200 and not r.get("busy"):
            return r
        time.sleep(0.1)
    return None


def wait_run(c, rid, until=("done", "failed", "stopped"), timeout=30, cond=None):
    t0 = time.time()
    while time.time() - t0 < timeout:
        st, r = c.get("/api/script_runs/" + rid)
        if st == 200 and (cond(r) if cond else r["status"] in until):
            return r
        time.sleep(0.05)
    return None


START = {"id": "s", "type": "start"}


def P(id, agent, text, **props):
    return {"id": id, "type": "prompt", "props": {"agent_id": agent, "text": text, **props}}


def script(name, nodes, edges, vars=None):
    return {"name": name, "nodes": nodes, "edges": edges, "vars": vars or []}


def run(c, s):
    st, saved = c.post("/api/scripts", s)
    assert st == 201, saved
    st, r = c.post(f"/api/scripts/{saved['id']}/run")
    return saved, st, r


def notices(c):
    return c.get("/api/notifications")[1]["items"]


def wait_notice(c, pred, timeout=5):
    t0 = time.time()
    while time.time() - t0 < timeout:
        hit = [n for n in notices(c) if pred(n)]
        if hit:
            return hit[0]
        time.sleep(0.05)
    return None


def budgets_part(owner):
    # -------------------------------------------------------------- budgets and Overview
    conv, st, r = chat(owner, "quill", "first")
    check("a chat works without limits", st == 200)
    wait_idle(owner, conv)
    st, o = owner.get("/api/overview?days=7")
    check("overview counts the run (demo = 15 tokens)", st == 200 and o["today"]["tokens"] == 15 and o["today"]["runs"] == 1, o.get("today"))
    check("overview has one row per day", len(o["by_day"]) == 7 and o["by_day"][-1]["tokens"] == 15, o.get("by_day"))
    check("overview per ghost", o["by_agent"][0]["id"] == "quill" and o["by_agent"][0]["tokens"] == 15)
    check("overview per person", o["by_user"][0]["id"] == "meto")
    check("overview per brain", o["by_brain"][0]["id"] == "demo")

    check("editor may not set budgets", EDITOR.put("/api/budgets", {"company": {"day_tokens": 1}})[0] == 403)
    st, o2 = EDITOR.get("/api/overview")
    check("editor sees only their own runs", st == 200 and o2["today"]["runs"] == 0 and "company" not in o2["budgets"], o2.get("today"))
    check("viewer has no overview", VIEWER.get("/api/overview")[0] == 403)

    st, b = owner.put("/api/budgets", {"company": {"day_tokens": 30, "day_usd": -4}, "agents": {"nobody": {"day_tokens": 5}}, "users": {"ece": {"month_tokens": "x"}}})
    check("budgets are cleaned", st == 200 and b["company"] == {"day_tokens": 30} and b["agents"] == {} and b["users"] == {}, b)
    conv, st, r = chat(owner, "quill", "second")
    wait_idle(owner, conv)
    check("under the limit a chat still runs", st == 200)
    conv, st, r = chat(owner, "quill", "third")
    check("over the limit a person is refused at once", st != 200 and r.get("error", "").startswith("Budget reached: the company's daily limit"), (st, r))
    n = wait_notice(owner, lambda x: x["kind"] == "budget")
    check("the owner gets a budget notice", n is not None and n["link"] == "#/overview")
    conv2, st, r = chat(EDITOR, "bolt", "again")
    time.sleep(0.2)
    check("one budget notice per period", len([x for x in notices(owner) if x["kind"] == "budget"]) == 1)
    check("editor is refused too", st != 200)

    # a hand-over or a script block is refused when it starts, and the failure reaches the asker
    saved, st, r = run(owner, script("Blocked", [START, P("p", "quill", "work")], [{"from": "s", "to": "p"}]))
    r = wait_run(owner, r["id"])
    check("a script block stops at the budget", r and r["status"] == "failed" and "Budget reached" in r["nodes"]["p"].get("error", ""), r and r["nodes"])
    saved, st, r = run(owner, script("Blocked retry", [START, P("p", "quill", "work", retry=3)], [{"from": "s", "to": "p"}]))
    r = wait_run(owner, r["id"])
    check("no retries against a spent budget", r and r["status"] == "failed" and r["nodes"]["p"].get("tries", 0) == 0, r and r["nodes"])

    st, o = owner.get("/api/overview")
    check("overview shows limit and use", o["budgets"]["company"]["limit"]["day_tokens"] == 30 and o["budgets"]["company"]["used"]["day_tokens"] >= 30, o["budgets"]["company"])
    owner.put("/api/budgets", {"users": {"ece": {"day_tokens": 1}}})
    conv, st, r = chat(owner, "quill", "owner has no limit now")
    check("a person's limit does not touch others", st == 200, (st, r))
    wait_idle(owner, conv)
    conv, st, r = chat(EDITOR, "quill", "one")
    check("first run under a person's limit", st == 200, (st, r))
    wait_idle(EDITOR, conv)
    conv, st, r = chat(EDITOR, "quill", "two")
    check("a person's limit stops only them", st != 200 and "Ece's daily limit" in r.get("error", ""), (st, r))
    owner.put("/api/budgets", {"agents": {"bolt": {"month_tokens": 1}}})
    conv, st, r = chat(owner, "bolt", "a")
    wait_idle(owner, conv)
    conv, st, r = chat(owner, "bolt", "b")
    check("a ghost's limit", st != 200 and "Bolt's monthly limit" in r.get("error", ""), (st, r))
    conv, st, r = chat(owner, "quill", "still fine")
    check("another ghost is not touched", st == 200, (st, r))
    wait_idle(owner, conv)
    owner.put("/api/budgets", {})


def notifications_part(owner):
    # -------------------------------------------------------------- notifications
    st, s = owner.get("/api/notifications/settings")
    check("settings have defaults", st == 200 and s["failed"] is True and s["done_min"] == 5)
    check("editor may not change notice settings", EDITOR.put("/api/notifications/settings", {"failed": False})[0] == 403)
    saved, st, r = run(EDITOR, script("Bad one", [START, P("p", "quill", "demo:fail")], [{"from": "s", "to": "p"}]))
    wait_run(EDITOR, r["id"])
    n = wait_notice(EDITOR, lambda x: x["kind"] == "failed" and "Bad one" in x["title"])
    check("a failed script tells the person who started it", n is not None and n["link"].endswith(r["id"]) and n["title_tr"].startswith("Betik başarısız"))
    check("nobody else sees that notice", wait_notice(VIEWER, lambda x: "Bad one" in x["title"], 0.3) is None)
    st, state = EDITOR.get("/api/state")
    check("the state carries the unread count", state.get("unread", 0) >= 1, state.get("unread"))
    EDITOR.post("/api/notifications/read", {"id": n["id"]})
    check("a notice can be marked read", [x for x in notices(EDITOR) if x["id"] == n["id"]][0]["read"] is True)
    EDITOR.post("/api/notifications/read")
    check("all can be marked read", EDITOR.get("/api/notifications")[1]["unread"] == 0)

    owner.put("/api/notifications/settings", {"done_min": 0})
    saved, st, r = run(owner, script("Fine one", [START, P("p", "quill", "hello")], [{"from": "s", "to": "p"}]))
    wait_run(owner, r["id"])
    check("a finished long script says so", wait_notice(owner, lambda x: x["kind"] == "done" and "Fine one" in x["title"]) is not None)
    conv, st, _ = chat(owner, "quill", "a chat")
    wait_idle(owner, conv)
    check("a finished long chat says so", wait_notice(owner, lambda x: x["kind"] == "done" and x["link"] == "#/chats/" + conv) is not None)
    owner.put("/api/notifications/settings", {"done": False, "done_min": 5})
    saved, st, r = run(owner, script("Quiet one", [START, P("p", "quill", "hello")], [{"from": "s", "to": "p"}]))
    wait_run(owner, r["id"])
    time.sleep(0.2)
    check("a switched-off kind stays silent", not [x for x in notices(owner) if "Quiet one" in x["title"]])
    owner.put("/api/notifications/settings", {"done": True})


def retry_part(owner):
    # -------------------------------------------------------------- retry and the failed arrow
    saved, st, r = run(owner, script("Retries", [START, P("p", "quill", "demo:fail", retry=2, retry_wait=1), {"id": "e", "type": "end"}],
                                     [{"from": "s", "to": "p"}, {"from": "p", "to": "e"}]))
    r2 = wait_run(owner, r["id"], cond=lambda x: x["nodes"]["p"].get("tries") == 1)
    check("waits between tries", r2 is not None and "retry_at" in r2["nodes"]["p"], r2 and r2["nodes"]["p"])
    r = wait_run(owner, r["id"])
    check("tries as often as it says, then fails the run", r and r["status"] == "failed" and len(r["nodes"]["p"]["runs"]) == 3 and r["nodes"]["p"]["tries"] == 2, r and r["nodes"]["p"])
    check("retries are logged", r and sum("Trying again" in e.get("text", "") for e in r["events"]) == 2)

    fn = [START, P("p", "quill", "demo:fail", retry=1, retry_wait=0), P("ok", "bolt", "worked"), P("bad", "bolt", "fix it"), {"id": "j", "type": "set", "props": {"var": "n", "expr": "n + 1"}}]
    fe = [{"from": "s", "to": "p"}, {"from": "p", "port": "out", "to": "ok"}, {"from": "p", "port": "failed", "to": "bad"}, {"from": "ok", "to": "j"}, {"from": "bad", "to": "j"}]
    saved, st, r = run(owner, script("Failed arrow", fn, fe, [{"name": "n", "type": "Number", "init": "0"}]))
    r = wait_run(owner, r["id"])
    check("the failed arrow keeps the run alive", r and r["status"] == "done", r and (r["status"], r.get("error")))
    check("only the failed side ran", r and r["nodes"]["bad"]["state"] == "done" and r["nodes"]["ok"]["state"] == "skipped", r and r["nodes"])
    check("a join after both sides still runs", r and r["nodes"]["j"]["state"] == "done")
    check("the block keeps its error", r and r["nodes"]["p"].get("took_failed") is True and "Demo failure" in r["nodes"]["p"].get("error", ""))

    nf = [START, P("p", "quill", "hello"), P("ok", "bolt", "worked"), P("bad", "bolt", "fix it")]
    ne = [{"from": "s", "to": "p"}, {"from": "p", "to": "ok"}, {"from": "p", "port": "failed", "to": "bad"}]
    saved, st, r = run(owner, script("No failure", nf, ne))
    r = wait_run(owner, r["id"])
    check("a good answer takes the normal arrow", r and r["status"] == "done" and r["nodes"]["ok"]["state"] == "done" and r["nodes"]["bad"]["state"] == "skipped", r and r["nodes"])

    # inside a loop every round starts with fresh tries
    ln = [START, {"id": "d", "type": "do", "props": {"count": "2"}}, P("p", "quill", "demo:fail", retry=1, retry_wait=0), P("h", "bolt", "handled")]
    le = [{"from": "s", "to": "d"}, {"from": "d", "port": "loop", "to": "p"}, {"from": "p", "port": "failed", "to": "h"}]
    saved, st, r = run(owner, script("Loop retry", ln, le))
    r = wait_run(owner, r["id"])
    check("every round gets its own tries", r and r["status"] == "done" and len(r["nodes"]["p"]["runs"]) == 4, r and (r["status"], r["nodes"]["p"]))

    st, c = owner.post("/api/scripts/validate", script("x", [START, {"id": "v", "type": "set", "props": {"var": "n", "expr": "1"}}, P("t", "quill", "x")],
                                                       [{"from": "s", "to": "v"}, {"from": "v", "port": "failed", "to": "t"}], [{"name": "n", "type": "Number"}]))
    check("only Prompt blocks have a failed output", any("no output 'failed'" in e["message"] for e in c["errors"]), c)
    st, c = owner.post("/api/scripts/validate", script("x", [START, P("p", "quill", "x", retry=99, retry_wait=-5)], [{"from": "s", "to": "p"}]))
    check("a script with extreme retry values is still valid", c["errors"] == [], c)
    st, saved = owner.post("/api/scripts", script("Clamp", [START, P("p", "quill", "x", retry="99", retry_wait="-5")], [{"from": "s", "to": "p"}]))
    props = [n for n in saved["nodes"] if n["id"] == "p"][0]["props"]
    check("retry values are clamped", props["retry"] == 5 and props["retry_wait"] == 0, props)


def ask_part(owner):
    # -------------------------------------------------------------- the Ask block
    nodes = [START, {"id": "a", "type": "ask", "props": {"question": "Publish {n}?"}}, P("yes", "quill", "go"), P("no", "bolt", "stop")]
    edges = [{"from": "s", "to": "a"}, {"from": "a", "port": "approved", "to": "yes"}, {"from": "a", "port": "rejected", "to": "no"}]
    v = [{"name": "n", "type": "Number", "init": "7"}]
    saved, st, r = run(EDITOR, script("Ask approve", nodes, edges, v))
    rid = r["id"]
    r = wait_run(EDITOR, rid, cond=lambda x: x["nodes"]["a"]["state"] == "running")
    check("an Ask block waits and fills in variables", r and r["status"] == "running" and r["nodes"]["a"]["question"] == "Publish 7.00?", r and r["nodes"]["a"])
    st, state = EDITOR.get("/api/state")
    check("the state lists the waiting question", len(state["asks"]) == 1 and state["asks"][0]["node"] == "a", state.get("asks"))
    check("the owner sees it too", len(owner.get("/api/state")[1]["asks"]) == 1)
    n = wait_notice(EDITOR, lambda x: x["kind"] == "ask" and "Ask approve" in x["title"])
    check("the person gets an ask notice with the question", n is not None and n["text"] == "Publish 7.00?")
    check("a viewer cannot answer", VIEWER.post(f"/api/script_runs/{rid}/answer", {"node": "a", "approved": True})[0] == 403)
    st, r = EDITOR.post(f"/api/script_runs/{rid}/answer", {"node": "nope", "approved": True})
    check("a wrong block is refused", st == 409)
    st, r = EDITOR.post(f"/api/script_runs/{rid}/answer", {"node": "a", "approved": True})
    check("approving goes on", st == 200, (st, r))
    r = wait_run(EDITOR, rid)
    check("the approved side ran, the other not", r and r["status"] == "done" and r["nodes"]["yes"]["state"] == "done" and r["nodes"]["no"]["state"] == "skipped", r and r["nodes"])
    check("who answered is kept", r and r["nodes"]["a"]["answered_by"] == "Ece")
    check("an old question cannot be answered", EDITOR.post(f"/api/script_runs/{rid}/answer", {"node": "a", "approved": True})[0] == 409)
    st, state = EDITOR.get("/api/state")
    check("nothing is waiting any more", state["asks"] == [])

    saved, st, r = run(EDITOR, script("Ask reject", nodes, edges, v))
    wait_run(EDITOR, r["id"], cond=lambda x: x["nodes"]["a"]["state"] == "running")
    st, r2 = owner.post(f"/api/script_runs/{r['id']}/answer", {"node": "a", "approved": False})
    r = wait_run(EDITOR, r["id"])
    check("the owner can answer; rejecting takes the other side", r and r["status"] == "done" and r["nodes"]["no"]["state"] == "done" and r["nodes"]["yes"]["state"] == "skipped", r and r["nodes"])

    # someone else's run
    ece2 = Client()
    tok = owner.post("/api/users", {"name": "Ali", "role": "editor"})[1]["invite_token"]
    ece2.post("/api/auth/join", {"token": tok, "password": "ali password 1"})
    saved, st, r = run(EDITOR, script("Ask rights", nodes, edges, v))
    wait_run(EDITOR, r["id"], cond=lambda x: x["nodes"]["a"]["state"] == "running")
    check("another editor cannot answer", ece2.post(f"/api/script_runs/{r['id']}/answer", {"node": "a", "approved": True})[0] == 409)
    owner.post(f"/api/script_runs/{r['id']}/stop")

    vn = [START, {"id": "a", "type": "ask", "props": {"question": "How many?", "mode": "value", "var": "n"}}, P("p", "quill", "count {n}")]
    ve = [{"from": "s", "to": "a"}, {"from": "a", "port": "approved", "to": "p"}]
    saved, st, r = run(EDITOR, script("Ask value", vn, ve, v))
    rid = r["id"]
    wait_run(EDITOR, rid, cond=lambda x: x["nodes"]["a"]["state"] == "running")
    st, e = EDITOR.post(f"/api/script_runs/{rid}/answer", {"node": "a", "approved": True, "value": "many"})
    check("a value of the wrong type is refused and it keeps waiting", st == 409 and "number" in e.get("error", ""), (st, e))
    st, r = EDITOR.post(f"/api/script_runs/{rid}/answer", {"node": "a", "approved": True, "value": "12,5"})
    r = wait_run(EDITOR, rid)
    check("a value goes into the variable", r and r["status"] == "done" and r["vars"]["n"]["value"] == 1250, r and r["vars"])
    check("the next block uses it", r and "count 12.50" in r["nodes"]["p"].get("output", ""), r and r["nodes"]["p"])

    tn = [START, {"id": "a", "type": "ask", "props": {"question": "Quick?", "timeout": 1}}, P("yes", "quill", "go"), P("no", "bolt", "late")]
    saved, st, r = run(EDITOR, script("Ask timeout", tn, edges))
    r = wait_run(EDITOR, r["id"])
    check("no answer in time takes the rejected side", r and r["status"] == "done" and r["nodes"]["no"]["state"] == "done" and r["nodes"]["a"]["output"] == "no answer in time", r and r["nodes"])

    for bad, want in [({"question": ""}, "question is empty"), ({"question": "x {nope}"}, "Unknown variable"), ({"question": "x", "mode": "value"}, "Pick a variable")]:
        st, c = owner.post("/api/scripts/validate", script("x", [START, {"id": "a", "type": "ask", "props": bad}], [{"from": "s", "to": "a"}]))
        check(f"Ask check: {want}", any(want in e["message"] for e in c["errors"]), c)
    st, c = owner.post("/api/scripts/validate", script("x", [START, {"id": "a", "type": "ask", "props": {"question": "q"}}, P("p", "quill", "x")], [{"from": "s", "to": "a"}, {"from": "a", "to": "p"}]))
    check("an Ask block has only approved and rejected outputs", any("no output 'out'" in e["message"] for e in c["errors"]), c)


def search_part(owner):
    # -------------------------------------------------------------- search
    conv, st, _ = chat(owner, "quill", "Şirketin GİZLİ çözümü burada: mavi kelebek")
    wait_idle(owner, conv)
    st, r = owner.get("/api/search?q=" + urllib.parse.quote("mavi kelebek"))
    hit = [x for x in r["results"] if x["link"] == "#/chats/" + conv]
    check("search finds a chat line", st == 200 and hit and hit[0]["match"].lower() == "mavi kelebek" and hit[0]["type"] == "chat", r)
    r = owner.get("/api/search?q=" + urllib.parse.quote("gizli cozumu"))[1]["results"]
    check("search folds case and Turkish letters", [x for x in r if x["link"] == "#/chats/" + conv and x["match"] == "GİZLİ çözümü"], r)
    r = EDITOR.get("/api/search?q=" + urllib.parse.quote("mavi kelebek"))[1]["results"]
    check("a private chat stays out of other people's search", not [x for x in r if x["link"] == "#/chats/" + conv], r)
    r = owner.get("/api/search?q=a")[1]
    check("too short a search finds nothing", r["results"] == [])

    owner.post("/api/projects", {"name": "Depo", "summary": "turuncu proje"})
    pid = owner.get("/api/state")[1]["projects"][0]["id"]
    owner.post(f"/api/projects/{pid}/notes", {"name": "Not", "body": "ambar kapisi şifresiz"})
    r = EDITOR.get("/api/search?q=" + urllib.parse.quote("ambar kapısı"))[1]["results"]
    check("search finds project notes", [x for x in r if x["type"] == "note" and x["link"] == "#/projects/" + pid], r)
    st, t = owner.post("/api/tasks", {"title": "Yeşil rapor", "description": "dördüncü çeyrek", "agent_id": "quill"})
    wait_idle(owner, t["conversation_id"]) if st == 201 else None
    r = owner.get("/api/search?q=" + urllib.parse.quote("dorduncu"))[1]["results"]
    check("search finds tasks", [x for x in r if x["type"] == "task"], (st, r))
    conv, st, _ = chat(owner, "quill", "demo:fail")
    wait_idle(owner, conv)
    r = owner.get("/api/search?q=" + urllib.parse.quote("failure requested"))[1]["results"]
    check("search finds the errors of runs", [x for x in r if x["type"] == "run"], r)
    mine = [x["id"] for x in owner.get("/api/runs")[1]["runs"] if x["conversation_id"] == conv]
    seen = [x["link"] for x in EDITOR.get("/api/search?q=" + urllib.parse.quote("failure requested"))[1]["results"]]
    check("the error of someone's private run stays hidden", mine and "#/runs/" + mine[0] not in seen and "#/chats/" + conv not in seen, (mine, seen))


def backup_part(owner, vault):
    # -------------------------------------------------------------- backup
    os.makedirs(os.path.join(vault, "Shared_Files"), exist_ok=True)
    with open(os.path.join(vault, "Shared_Files", "büyük_dosya.txt"), "w", encoding="utf-8") as f:
        f.write("paylaşılan " * 1000)
    pid = owner.get("/api/state")[1]["projects"][0]["id"]
    with open(os.path.join(vault, "Projects", pid, "work.txt"), "w", encoding="utf-8") as f:
        f.write("work file")

    def make(include):
        st, r = owner.post("/api/backup", {"include_files": include})
        check("a backup starts", st == 202, (st, r))
        t0 = time.time()
        while time.time() - t0 < 20:
            r = owner.get("/api/backup")[1]
            if not r["running"]:
                break
            time.sleep(0.05)
        check("the backup has no error", "error" not in r["last"], r["last"])
        return r["last"]["name"]

    def fetch(name):
        st, data, headers = owner.raw("GET", "/api/backup/download?name=" + urllib.parse.quote(name))
        return st, data, headers

    check("editor cannot make backups", EDITOR.post("/api/backup", {})[0] == 403)
    check("editor cannot list backups", EDITOR.get("/api/backup")[0] == 403)
    full = make(True)
    st, data, headers = fetch(full)
    check("a backup can be downloaded", st == 200 and "attachment" in headers.get("Content-Disposition", ""))
    z = zipfile.ZipFile(io.BytesIO(data))
    check("the zip is intact", z.testzip() is None)
    names = set(z.namelist())
    check("it holds the Vault", "vault.json" in names and any(n.startswith("Conversations/") for n in names) and any(n.startswith("Agents/") for n in names), sorted(names)[:8])
    check("Turkish file names stay right", "Shared_Files/büyük_dosya.txt" in names, sorted(names)[:40])
    check("a full backup has the work files", f"Projects/{pid}/work.txt" in names)
    check("content is right", z.read("Shared_Files/büyük_dosya.txt").decode("utf-8") == "paylaşılan " * 1000)
    check("project notes are in it", any(n.startswith(f"Projects/{pid}/.Phantom/") for n in names))
    check("the lock file is left out", ".phantom_lock" not in names)
    check("no password file in the Vault", not any("auth.json" in n or "secrets.json" in n for n in names))

    time.sleep(1.1)  # names differ by the second
    data_only = make(False)
    st, data, _ = fetch(data_only)
    z = zipfile.ZipFile(io.BytesIO(data))
    names = set(z.namelist())
    check("a data backup leaves out work files", f"Projects/{pid}/work.txt" not in names and not any(n.startswith("Shared_Files/") for n in names), sorted(names)[:40])
    check("a data backup keeps project notes", any(n.startswith(f"Projects/{pid}/.Phantom/") for n in names))
    check("a data backup is intact", z.testzip() is None and "vault.json" in names)
    lst = owner.get("/api/backup")[1]["backups"]
    check("backups are listed, newest first", [b["name"] for b in lst] == [data_only, full] and lst[0]["full"] is False, lst)
    check("a wrong name is refused", fetch("..\\..\\auth.json")[0] == 404 and fetch("Phantom_Backup_x.zip")[0] == 404)
    check("editor cannot download", EDITOR.raw("GET", "/api/backup/download?name=" + full)[0] == 403)
    check("a backup can be deleted", owner.delete("/api/backup?name=" + full)[0] == 200 and len(owner.get("/api/backup")[1]["backups"]) == 1)


def export_part(owner):
    # -------------------------------------------------------------- chat export
    conv, st, _ = chat(owner, "quill", "Dışa aktarılacak satır\nikinci satır")
    wait_idle(owner, conv)
    st, data, headers = owner.raw("GET", f"/api/chats/{conv}/export")
    text = data.decode("utf-8")
    check("a chat can be exported", st == 200 and headers.get("Content-Type", "").startswith("text/markdown") and "attachment" in headers.get("Content-Disposition", ""), headers)
    check("the export names who said what", "**Meto** · " in text and "Dışa aktarılacak satır\nikinci satır" in text and "**Quill** · " in text and "Boo! Quill here" in text, text[:400])
    check("a private chat cannot be exported by others", EDITOR.raw("GET", f"/api/chats/{conv}/export")[0] == 404)
    owner.put("/api/chats/" + conv, {"shared": True})
    check("a shared chat can be exported by the team", EDITOR.raw("GET", f"/api/chats/{conv}/export")[0] == 200)


def memory_part(owner, vault):
    # -------------------------------------------------------------- ghost memory
    conv, st, _ = chat(owner, "quill", "demo:remember:Should stay plain")
    r = wait_idle(owner, conv)
    check("without memory the tag is plain text", any("<remember>" in m.get("text", "") for m in r["messages"]), r["messages"])
    check("without memory nothing is saved", owner.get("/api/agents/quill/memory")[1]["notes"] == [])

    a = owner.get("/api/state")[1]["agents"]
    pip = owner.post("/api/agents", {"name": "Pip", "provider": "demo", "memory": True})[1]
    check("a ghost can have memory", pip.get("memory") is True, pip)
    conv, st, _ = chat(owner, "pip", "demo:remember:Meto likes short answers")
    r = wait_idle(owner, conv)
    check("the tag is taken out of the answer", not any("<remember>" in m.get("text", "") for m in r["messages"]), r["messages"])
    check("the person sees what was saved", any(m.get("text") == "🧠 Remembered: Meto likes short answers" for m in r["messages"]), r["messages"])
    st, m = owner.get("/api/agents/pip/memory")
    check("the note is stored", m["notes"] == ["Meto likes short answers"], m)
    check("it is a plain file in the Vault", os.path.exists(os.path.join(vault, "Memory", "pip", "meto.md")))

    conv2, st, _ = chat(owner, "pip", "demo:prompt")
    r = wait_idle(owner, conv2)
    text = " ".join(x.get("text", "") for x in r["messages"])
    check("a new chat has the note in its prompt", "## Your memory" in text and "1. Meto likes short answers" in text, text[:1500])

    conv3, st, _ = chat(EDITOR, "pip", "demo:prompt")
    r = wait_idle(EDITOR, conv3)
    text = " ".join(x.get("text", "") for x in r["messages"])
    check("another person never gets my notes", "## Your memory" in text and "Meto likes" not in text, text[:1500])
    check("another person's list is empty", EDITOR.get("/api/agents/pip/memory")[1]["notes"] == [])
    conv4, st, _ = chat(owner, "quill", "demo:prompt")
    r = wait_idle(owner, conv4)
    check("a ghost without memory gets no memory part", "## Your memory" not in " ".join(x.get("text", "") for x in r["messages"]))

    conv5, st, _ = chat(owner, "pip", "demo:remember:Second note\ndemo:forget:1")
    r = wait_idle(owner, conv5)
    check("a ghost can forget a note", owner.get("/api/agents/pip/memory")[1]["notes"] == ["Second note"], owner.get("/api/agents/pip/memory")[1])
    check("forgetting is shown too", any(m.get("text") == "🧠 Forgot: Meto likes short answers" for m in r["messages"]), r["messages"])

    check("the same note is not kept twice", owner.post("/api/agents/pip/memory", {"text": "Second note"})[0] == 400)
    st, m = owner.post("/api/agents/pip/memory", {"text": "line one\n\n  line two"})
    check("a person can add a note, on one line", st == 201 and m["notes"][-1] == "line one line two", m)
    st, m = owner.post("/api/agents/pip/memory", {"text": "ş" * 400})
    check("a note is cut at a character", st == 201 and m["notes"][-1] == "ş" * 150 + "…", m["notes"][-1])
    st, m = owner.delete("/api/agents/pip/memory/1")
    check("a person can delete a note", st == 200 and m["notes"][0] == "line one line two", m)
    check("a wrong number is refused", owner.delete("/api/agents/pip/memory/99")[0] == 404)
    for i in range(45):
        owner.post("/api/agents/pip/memory", {"text": f"fact {i}"})
    m = owner.get("/api/agents/pip/memory")[1]
    check("old notes go when it is full", len(m["notes"]) == 40 and m["notes"][-1] == "fact 44" and "fact 0" not in m["notes"], len(m["notes"]))
    check("viewers have no memory access", VIEWER.get("/api/agents/pip/memory")[0] == 403)
    EDITOR.post("/api/agents/pip/memory", {"text": "ece's note"})
    owner.delete("/api/agents/pip/memory")
    check("clearing is per person", owner.get("/api/agents/pip/memory")[1]["notes"] == [] and EDITOR.get("/api/agents/pip/memory")[1]["notes"] == ["ece's note"])
    check("firing a ghost removes its notes", owner.delete("/api/agents/pip")[0] == 200 and not os.path.exists(os.path.join(vault, "Memory", "pip")))


def restart_part(exe, vault, data, env):
    # -------------------------------------------------------------- an Ask block survives a restart (also the settings)
    owner = Client()
    owner.post("/api/auth/login", {"name": "Meto", "password": "correct horse 1"})
    nodes = [START, {"id": "a", "type": "ask", "props": {"question": "After restart?"}}, P("yes", "quill", "go")]
    edges = [{"from": "s", "to": "a"}, {"from": "a", "port": "approved", "to": "yes"}]
    saved, st, r = run(owner, script("Ask restart", nodes, edges))
    rid = r["id"]
    wait_run(owner, rid, cond=lambda x: x["nodes"]["a"]["state"] == "running")
    owner.put("/api/budgets", {"company": {"month_usd": 500}})
    owner.put("/api/notifications/settings", {"done_min": 17})
    return owner, rid


def main():
    global EDITOR, VIEWER
    exe = sys.argv[1]
    tmp = tempfile.mkdtemp(prefix="pc2_unattended_")
    vault, data = os.path.join(tmp, "Vault"), os.path.join(tmp, "Data")
    env = dict(os.environ, PHANTOM_SCRIPT_MINUTE_MS="100", PHANTOM_USAGE_PROBE="off")
    args = [exe, "--vault", vault, "--data", data, "--port", str(PORT), "--no-window"]

    def wait_up():
        for _ in range(100):
            try:
                urllib.request.urlopen(BASE + "/api/auth/status", timeout=1)
                return
            except Exception:
                time.sleep(0.1)
        raise RuntimeError("program did not start")

    proc = subprocess.Popen(args, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        wait_up()
        owner, EDITOR, VIEWER = Client(), Client(), Client()
        owner.post("/api/auth/setup", {"name": "Meto", "password": "correct horse 1"})
        tok_e = owner.post("/api/users", {"name": "Ece", "role": "editor"})[1]["invite_token"]
        tok_v = owner.post("/api/users", {"name": "Vera", "role": "viewer"})[1]["invite_token"]
        EDITOR.post("/api/auth/join", {"token": tok_e, "password": "ece password 1"})
        VIEWER.post("/api/auth/join", {"token": tok_v, "password": "vera password 1"})
        for name in ("Quill", "Bolt"):
            assert owner.post("/api/agents", {"name": name, "provider": "demo"})[0] == 201

        budgets_part(owner)
        notifications_part(owner)
        retry_part(owner)
        ask_part(owner)
        search_part(owner)
        backup_part(owner, vault)
        export_part(owner)
        memory_part(owner, vault)

        o2, rid = restart_part(exe, vault, data, env)
        proc.terminate()
        proc.wait(20)
        proc = subprocess.Popen(args, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        wait_up()
        o3 = Client()
        o3.post("/api/auth/login", {"name": "Meto", "password": "correct horse 1"})
        st, r = o3.get("/api/script_runs/" + rid)
        check("an Ask block still waits after a restart", st == 200 and r["status"] == "running" and r["nodes"]["a"]["state"] == "running", r.get("nodes"))
        check("it is not asked twice", sum(1 for n in o3.get("/api/notifications")[1]["items"] if "Ask restart" in n["title"]) == 1)
        o3.post(f"/api/script_runs/{rid}/answer", {"node": "a", "approved": True})
        r = wait_run(o3, rid)
        check("it can be answered after a restart", r and r["status"] == "done" and r["nodes"]["yes"]["state"] == "done", r and r["nodes"])
        st, o = o3.get("/api/overview")
        check("budgets survive a restart", o["budgets"]["company"]["limit"].get("month_usd") == 500, o["budgets"]["company"])
        check("notice settings survive a restart", o3.get("/api/notifications/settings")[1]["done_min"] == 17)

        if "--real" in sys.argv:  # a real Haiku ghost: the memory tags work, and real runs are counted (a little subscription quota)
            st, g = o3.post("/api/agents", {"name": "Mira", "provider": "claude", "model": "haiku", "memory": True})
            check("real ghost with memory", st == 201, g)
            conv, st, _ = chat(o3, g["id"], "Please remember for later chats that my favourite colour is teal. Then just say ok.")
            r = wait_idle(o3, conv, 180)
            saved = o3.get(f"/api/agents/{g['id']}/memory")[1]["notes"]
            check("a real ghost saves a note", any("teal" in n.lower() for n in saved), (saved, r and r["messages"]))
            check("the tag is not shown to the person", r and not any("<remember>" in m.get("text", "") for m in r["messages"]))
            conv, st, _ = chat(o3, g["id"], "What is my favourite colour? One word.")
            r = wait_idle(o3, conv, 180)
            last = [m.get("text", "") for m in r["messages"] if m.get("from", "").startswith("agent:")] if r else []
            check("a new chat knows it", last and "teal" in last[-1].lower(), last)
            t = o3.get("/api/overview")[1]["today"]
            check("real tokens and cost are counted", t["tokens"] > 100 and t["cost"] > 0, t)
    finally:
        proc.terminate()
        try:
            proc.wait(20)
        except Exception:
            proc.kill()
        shutil.rmtree(tmp, ignore_errors=True)
    print(f"\n{'ALL PASSED' if not failures else str(len(failures)) + ' FAILED: ' + ', '.join(failures)}")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
