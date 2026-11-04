"""Smoke test for the Phantom Co. Node: starts it on a temporary Vault and data folder and walks through the API.

Usage: python smoke_test.py <path-to-pc2_node.exe> [--real claude|codex]
The default run only uses the built-in "demo" provider (costs nothing).
"""
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

PORT = 7791
BASE = f"http://127.0.0.1:{PORT}"
failures = []


def check(name, cond, extra=""):
    print(("PASS " if cond else "FAIL ") + name + (f"  [{extra}]" if extra and not cond else ""))
    if not cond:
        failures.append(name)


class Client:
    """One signed-in browser: keeps its session cookie."""

    def __init__(self):
        self.cookie = ""

    def call(self, method, path, body=None, headers=None):
        data = json.dumps(body).encode() if body is not None else None
        h = {"X-PC2": "1"}
        if body is not None:
            h["Content-Type"] = "application/json"
        if self.cookie:
            h["Cookie"] = self.cookie
        h.update(headers or {})
        req = urllib.request.Request(BASE + path, data=data, method=method, headers=h)
        try:
            with urllib.request.urlopen(req, timeout=15) as r:
                raw = r.read()
                sc = r.headers.get("Set-Cookie")
                if sc and "pc2_session_" in sc:
                    tok = sc.split("pc2_session_" + str(PORT) + "=")[1].split(";")[0]
                    self.cookie = f"pc2_session_{PORT}={tok}" if tok else ""
                return r.status, (json.loads(raw) if raw[:1] in (b"{", b"[") else raw)
        except urllib.error.HTTPError as e:
            raw = e.read()
            try:
                return e.code, json.loads(raw)
            except Exception:
                return e.code, raw

    def get(self, p, **kw): return self.call("GET", p, **kw)
    def post(self, p, b=None, **kw): return self.call("POST", p, b if b is not None else {}, **kw)
    def put(self, p, b, **kw): return self.call("PUT", p, b, **kw)
    def delete(self, p, **kw): return self.call("DELETE", p, **kw)

    def wait_idle(self, chat_id, timeout=120):
        t0 = time.time()
        while time.time() - t0 < timeout:
            st, v = self.get("/api/chats/" + chat_id)
            if st == 200 and not v["busy"]:
                return v
            time.sleep(0.3)
        return None


def start(exe, vault, data, extra=()):
    proc = subprocess.Popen([exe, "--vault", vault, "--data", data, "--port", str(PORT), "--no-window", *extra],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    for _ in range(60):
        try:
            Client().get("/api/auth/status")
            return proc
        except Exception:
            time.sleep(0.2)
    return proc


def stop(proc):
    proc.terminate()
    try:
        proc.wait(5)
    except Exception:
        proc.kill()


def main():
    exe = sys.argv[1]
    real = sys.argv[sys.argv.index("--real") + 1] if "--real" in sys.argv else None
    tmp = tempfile.mkdtemp(prefix="pc2_smoke_")
    vault, data = os.path.join(tmp, "Vault"), os.path.join(tmp, "Data")
    proc = start(exe, vault, data)
    try:
        owner, ece, vera, anon = Client(), Client(), Client(), Client()

        # ---------------------------------------------------------- first start
        for p in ["vault.json", "README.md", "Company/company.json", "Agents", "Skills", "Rules", "Projects", "Tasks", "Task_Templates", "Conversations", "Runs", "Shared_Files"]:
            check("vault has " + p, os.path.exists(os.path.join(vault, p)))
        skills = sorted(os.listdir(os.path.join(vault, "Skills")))
        check("default skills installed", "read_files.md" in skills and "agent_management.md" in skills and "shell_commands.md" not in skills, skills)
        st, s = anon.get("/api/auth/status")
        check("setup needed on a fresh Node", st == 200 and s["setup_needed"] and s["me"] is None, s)
        check("api needs sign-in", anon.get("/api/state")[0] == 401)
        st, html = anon.get("/")
        check("ui served without sign-in", st == 200 and b"Phantom Co." in html)
        check("foreign Host rejected", anon.get("/api/auth/status", headers={"Host": "evil.example.com"})[0] == 403)
        check("missing X-PC2 rejected", anon.post("/api/auth/login", {"name": "a", "password": "b"}, headers={"X-PC2": "0"})[0] == 403)
        check("foreign Origin rejected", anon.post("/api/auth/login", {"name": "a", "password": "b"}, headers={"Origin": "http://evil.example.com"})[0] == 403)

        # ---------------------------------------------------------------- owner
        check("short password rejected", owner.post("/api/auth/setup", {"name": "Meto", "password": "short"})[0] == 400)
        st, me = owner.post("/api/auth/setup", {"name": "Meto Y", "password": "correct horse 1"})
        check("owner created and signed in", st == 200 and me["role"] == "owner" and me["id"] == "meto_y", me)
        check("second setup refused", anon.post("/api/auth/setup", {"name": "X", "password": "correct horse 1"})[0] == 409)
        check("user profile file", os.path.exists(os.path.join(vault, "Users", "meto_y.md")))
        check("password hash not in vault", "hash" not in open(os.path.join(vault, "Users", "meto_y.md"), encoding="utf-8").read())
        check("auth file outside vault", os.path.exists(os.path.join(data, "auth.json")))
        st, state = owner.get("/api/state")
        check("owner state has settings", st == 200 and "settings" in state and state["me"]["role"] == "owner")
        st, s = anon.get("/api/auth/status")
        check("status tells the version", st == 200 and re.fullmatch(r"\d+\.\d+\.\d+", s.get("version", "")), s)
        st, ab = owner.put("/api/about", {"check_updates": False})  # no GitHub call from the test
        check("update check can be switched off", st == 200 and ab["check_updates"] is False and "update" not in ab, ab)
        st, ab = owner.get("/api/about")
        check("about shows version, no update check when off", st == 200 and ab["version"] == s["version"] and "update" not in ab, ab)
        check("update setting saved in node.json", json.load(open(os.path.join(data, "node.json"), encoding="utf-8")).get("check_updates") is False)

        check("wrong password", Client().post("/api/auth/login", {"name": "meto y", "password": "nope nope 1"})[0] == 401)
        c2 = Client()
        st, _ = c2.post("/api/auth/login", {"name": "METO Y", "password": "correct horse 1"})
        check("login case-insensitive", st == 200 and c2.get("/api/state")[0] == 200)
        thr = Client()
        codes = [thr.post("/api/auth/login", {"name": "ghosty", "password": "x" * 9})[0] for _ in range(7)]
        check("login throttled after repeated failures", codes[-1] == 429, codes)

        # ----------------------------------------------------------------- team
        st, r = owner.post("/api/users", {"name": "Ece", "role": "editor"})
        check("invite editor", st == 201 and len(r["invite_token"]) == 48, r)
        ece_tok = r["invite_token"]
        st, r = owner.post("/api/users", {"name": "Vera", "role": "viewer"})
        vera_tok = r["invite_token"]
        check("duplicate name refused", owner.post("/api/users", {"name": "ece", "role": "viewer"})[0] == 409)
        check("invite shows name", anon.get("/api/auth/invite?token=" + ece_tok)[1].get("name") == "Ece")
        check("bad invite refused", anon.get("/api/auth/invite?token=" + "0" * 48)[0] == 404)
        check("join sets password", ece.post("/api/auth/join", {"token": ece_tok, "password": "ece password 1"})[0] == 200)
        check("invite works once", Client().post("/api/auth/join", {"token": ece_tok, "password": "other pass 12"})[0] == 404)
        vera.post("/api/auth/join", {"token": vera_tok, "password": "vera password 1"})
        st, state = ece.get("/api/state")
        check("editor state has no settings", st == 200 and "settings" not in state and "vault_path" not in state)
        check("editor cannot list users", ece.get("/api/users")[0] == 403)
        check("editor cannot create skills", ece.post("/api/skills", {"name": "X", "tools": ["Bash"]})[0] == 403)
        check("viewer cannot create agents", vera.post("/api/agents", {"name": "Nope"})[0] == 403)
        check("viewer can read state", vera.get("/api/state")[0] == 200)
        check("last owner protected", owner.put("/api/users/meto_y", {"role": "editor"})[0] == 409)
        # presence: a user with a recent request is online; signing out puts them to sleep at once
        pres = lambda: {u["id"]: u for u in owner.get("/api/state")[1]["users"]}
        check("signed-in users online", pres()["ece"]["online"] and pres()["meto_y"]["online"] and "last_seen" not in pres()["ece"], pres())
        v2 = Client()
        v2.post("/api/auth/login", {"name": "Vera", "password": "vera password 1"})
        v2.post("/api/auth/logout")
        p = pres()["vera"]
        check("signed-out user asleep", p["online"] is False and p.get("last_seen", "").endswith("Z"), p)
        vera.get("/api/state")
        check("back online after a request", pres()["vera"]["online"], pres()["vera"])

        # ------------------------------------------------- accounts (Settings > Secrets)
        check("no accounts at first", owner.get("/api/state")[1]["accounts"] == [])
        check("editor cannot add accounts", ece.post("/api/accounts", {"provider": "claude", "name": "Work"})[0] == 403)
        check("unknown account provider refused", owner.post("/api/accounts", {"provider": "demo", "name": "X"})[0] == 400)
        st, acct = owner.post("/api/accounts", {"provider": "claude", "name": "Work"})
        check("account added", st == 201 and acct["id"].startswith("acct_") and acct["provider"] == "claude", acct)
        check("account folder outside the vault", os.path.isdir(os.path.join(data, "Accounts", acct["id"])) and os.path.exists(os.path.join(data, "accounts.json")))
        check("editor sees account names", [a["name"] for a in ece.get("/api/state")[1]["accounts"]] == ["Work"])
        check("account renamed", owner.put("/api/accounts/" + acct["id"], {"name": "Job"})[1].get("name") == "Job")
        brain = "claude#" + acct["id"]
        st, ab = owner.post("/api/agents", {"name": "Acct Bot", "provider": brain})
        check("agent with an account", st == 201 and ab["provider"] == brain, ab)
        check("unknown account refused", owner.post("/api/agents", {"name": "Z", "provider": "claude#acct_nope"})[0] == 400)
        check("account of another provider refused", owner.post("/api/agents", {"name": "Z", "provider": "codex#" + acct["id"]})[0] == 400)
        check("demo with an account refused", owner.post("/api/agents", {"name": "Z", "provider": "demo#" + acct["id"]})[0] == 400)
        check("editor cannot read sign-in states", ece.get("/api/accounts/status")[0] == 403)
        st, ss = owner.get("/api/accounts/status")
        mine = [a for a in ss.get("accounts", []) if a["id"] == acct["id"]]
        check("sign-in states listed", st == 200 and [a["provider"] for a in ss["auto"]] == ["claude", "codex", "gemini"] and mine and mine[0]["agents"] == ["Acct Bot"], ss)
        if mine and mine[0]["status"].get("error") != "not_installed":
            check("new account is not signed in (own folder)", mine[0]["status"]["signed_in"] is False, mine[0]["status"])
        check("sign-in only at this PC", owner.post("/api/accounts/login", {"id": acct["id"]}, headers={"X-Forwarded-For": "1.2.3.4"})[0] == 403)
        st, r = owner.delete("/api/accounts/" + acct["id"])
        check("account removed, its ghosts back to Auto", st == 200 and r["moved"] == ["Acct Bot"] and owner.get("/api/state")[1]["accounts"] == [], r)
        ab_after = [a for a in owner.get("/api/state")[1]["agents"] if a["id"] == ab["id"]]
        check("ghost uses Auto", ab_after and ab_after[0]["provider"] == "claude", ab_after)
        check("account folder deleted", not os.path.exists(os.path.join(data, "Accounts", acct["id"])))
        owner.delete("/api/agents/" + ab["id"])
        # API keys: encrypted in secrets.json (DPAPI), only "…last4" in the account list, typed only at this PC
        fake = "sk-ant-api03-phantomtest-" + "x" * 20 + "WXYZ"
        check("key refused from the network", owner.post("/api/accounts", {"provider": "claude", "name": "Key", "kind": "api_key", "key": fake}, headers={"X-Forwarded-For": "1.2.3.4"})[0] == 403)
        check("bad key refused", owner.post("/api/accounts", {"provider": "claude", "name": "Key", "kind": "api_key", "key": "short"})[0] == 400)
        check("unknown kind refused", owner.post("/api/accounts", {"provider": "claude", "name": "Key", "kind": "magic"})[0] == 400)
        st, ka = owner.post("/api/accounts", {"provider": "claude", "name": "Key", "kind": "api_key", "key": fake})
        check("api key account added", st == 201 and ka["kind"] == "api_key" and ka["key_hint"] == "…WXYZ" and "key" not in ka, ka)
        on_disk = "".join(open(os.path.join(data, f), encoding="utf-8").read() for f in ("accounts.json", "secrets.json"))
        check("key never stored in plain text", fake not in on_disk and "phantomtest" not in on_disk and ka["id"] in on_disk)
        check("state shows the kind, not the key", [(a["kind"], "key_hint" in a) for a in ece.get("/api/state")[1]["accounts"]] == [("api_key", False)])
        check("nothing to sign in with a key", owner.post("/api/accounts/login", {"id": ka["id"]})[0] == 409)
        check("key replace refused from the network", owner.put("/api/accounts/" + ka["id"], {"key": fake + "2"}, headers={"X-Forwarded-For": "1.2.3.4"})[0] == 403)
        st, ka2 = owner.put("/api/accounts/" + ka["id"], {"key": fake[:-4] + "ABCD"})
        check("key replaced", st == 200 and ka2["key_hint"] == "…ABCD", ka2)
        st, ss = owner.get("/api/accounts/status")
        ks = [a for a in ss["accounts"] if a["id"] == ka["id"]][0]["status"]
        check("fake key not accepted", ks["signed_in"] is False and ks["error"] in ("key_rejected", "unreachable"), ks)
        owner.delete("/api/accounts/" + ka["id"])
        # Gemini CLI: a third brain, with accounts like the others; not lent by friends
        check("gemini is a known brain", "gemini" in [p["id"] for p in owner.get("/api/state")[1]["providers"]])
        st, gb = owner.post("/api/agents", {"name": "Gem Bot", "provider": "gemini"})
        check("gemini agent", st == 201 and gb["provider"] == "gemini", gb)
        check("gemini from a friend refused", owner.post("/api/agents", {"name": "Z", "provider": "gemini@ece"})[0] == 400)
        st, gsub = owner.post("/api/accounts", {"provider": "gemini", "name": "Google", "kind": "subscription"})
        check("gemini google sign-in account refused", st == 400 and "API key" in gsub.get("error", ""), gsub)
        check("gemini sign-in window refused", owner.post("/api/accounts/login", {"provider": "gemini"})[0] == 409)
        st, ga = owner.post("/api/accounts", {"provider": "gemini", "name": "Google", "kind": "api_key", "key": "AIzaPhantomTest" + "x" * 24})
        check("gemini key account", st == 201 and ga["provider"] == "gemini" and ga["key_hint"] == "…xxxx", ga)
        check("gemini agent with an account", owner.put("/api/agents/" + gb["id"], {"provider": "gemini#" + ga["id"]})[0] == 200)
        gs = [a for a in owner.get("/api/accounts/status")[1]["accounts"] if a["id"] == ga["id"]][0]["status"]
        check("fake gemini key not accepted", gs["signed_in"] is False and gs["error"] in ("key_rejected", "unreachable"), gs)
        owner.delete("/api/accounts/" + ga["id"])
        owner.delete("/api/agents/" + gb["id"])
        check("key removed with the account", ka["id"] not in open(os.path.join(data, "secrets.json"), encoding="utf-8").read())

        # --------------------------------------------------------------- agents
        st, casper =owner.post("/api/agents", {"name": "Casper", "role": "Planner", "provider": "demo", "form": "royal",
                                                "skills": ["read_files", "write_files", "nonexistent"], "persona": "Be brief."})
        check("agent created", st == 201 and casper["id"] == "casper" and casper["skills"] == ["read_files", "write_files"], casper)
        text = open(os.path.join(vault, "Agents", "casper.md"), encoding="utf-8").read()
        check("agent is a markdown file", text.startswith("---\n") and "skill: read_files" in text and text.rstrip().endswith("Be brief."), text)
        st, wisp = owner.post("/api/agents", {"name": "Wisp", "role": "Helper", "provider": "demo", "reports_to": "casper"})
        check("boss accepted", st == 201 and wisp["reports_to"] == "casper", wisp)
        check("boss loop refused", ece.put("/api/agents/casper", {"reports_to": "wisp"})[0] == 400)
        check("unknown boss refused", owner.post("/api/agents", {"name": "Z", "provider": "demo", "reports_to": "ghost_x"})[0] == 400)
        st, adm = owner.post("/api/agents", {"name": "Boss Bot", "provider": "demo", "skills": ["write_files", "agent_management"]})

        # ---------------------------------------------------------------- chats
        st, chat = ece.post("/api/chats", {"agent_id": "casper"})
        check("chat created", st == 201 and chat["meta"]["user_id"] == "ece" and chat["can_write"], chat)
        cid = chat["meta"]["id"]
        check("viewer cannot start chats", vera.post("/api/chats", {"agent_id": "casper"})[0] == 403)
        st, v = ece.post(f"/api/chats/{cid}/send", {"text": "hello demo:write"})
        check("send accepted and busy", st == 200 and v["busy"], v)
        check("second send while busy refused", ece.post(f"/api/chats/{cid}/send", {"text": "again"})[0] == 409)
        v = ece.wait_idle(cid)
        froms = [m["from"] for m in v["messages"]]
        check("agent answered", "user:ece" in froms and "agent:casper" in froms, froms)
        check("title from the agent", v["meta"]["title"] == "Demo chat", v["meta"])
        check("agent wrote file in shared folder", any(f.startswith("demo_") for f in os.listdir(os.path.join(vault, "Shared_Files"))))
        check("private chat hidden from other editor/viewer", vera.get("/api/chats/" + cid)[0] == 404)
        check("owner sees every chat", owner.get("/api/chats/" + cid)[0] == 200)
        check("chat list private", all(c["id"] != cid for c in vera.get("/api/chats")[1]["chats"]))
        ece.put("/api/chats/" + cid, {"shared": True})
        check("shared chat visible to viewer", vera.get("/api/chats/" + cid)[0] == 200)
        v = vera.get("/api/chats/" + cid)[1]
        check("viewer cannot write", not v["can_write"] and vera.post(f"/api/chats/{cid}/send", {"text": "hi"})[0] == 403)
        check("run record in Vault", len(os.listdir(os.path.join(vault, "Runs"))) >= 2)

        # session resume + reset
        st, v = ece.post(f"/api/chats/{cid}/send", {"text": "second"})
        v = ece.wait_idle(cid)
        check("shared chat prefixes the writer", any("(Ece)" in m.get("text", "") for m in v["messages"] if m["from"] == "agent:casper"), v["messages"][-1])

        # agent to agent
        st, chat2 = ece.post("/api/chats", {"agent_id": "casper"})
        c2id = chat2["meta"]["id"]
        ece.post(f"/api/chats/{c2id}/send", {"text": "please ask demo:send:wisp"})
        v = ece.wait_idle(c2id)
        texts = [m["text"] for m in v["messages"]]
        check("chat stays busy until the chain ended", v is not None)
        check("report came back and was answered", any("Thanks, I got the report" in t for t in texts), texts)
        check("system lines show the hand-over", any("asked Wisp" in t for t in texts) and any("reported back" in t for t in texts), texts)
        convs = ece.get("/api/chats")[1]["chats"]
        agents_conv = [c for c in convs if c["kind"] == "agents"]
        check("agent-to-agent conversation exists", len(agents_conv) == 1 and agents_conv[0]["title"] == "Demo request", agents_conv)
        x = ece.get("/api/chats/" + agents_conv[0]["id"])[1]
        check("agent conversation has both sides", [m["from"] for m in x["messages"]] == ["agent:casper", "agent:wisp"], x["messages"])
        check("agent conversation read-only", not x["can_write"])
        check("other users do not see it", all(c["kind"] != "agents" for c in vera.get("/api/chats")[1]["chats"]))
        # information only (reply="no"): the asker is not woken up, but the chat shows how the work ended
        st, c3 = ece.post("/api/chats", {"agent_id": "casper"})
        ece.post(f"/api/chats/{c3['meta']['id']}/send", {"text": "just tell demo:tell:wisp"})
        v = ece.wait_idle(c3["meta"]["id"])
        texts = [m["text"] for m in v["messages"]]
        check("information-only send marked", any("asked Wisp (information only)" in t for t in texts), texts)
        check("information-only result shown without a report run",
              any(t.startswith("ℹ Wisp finished (Casper did not ask for an answer): Boo! Wisp here.") for t in texts)
              and not any("Thanks, I got the report" in t or "reported back" in t for t in texts), texts)
        st, state = ece.get("/api/runs")
        kinds = {r["kind"] for r in state["runs"]}
        check("run kinds recorded", {"chat", "msg", "report"} <= kinds, kinds)

        # stop and failure
        st, chat3 = ece.post("/api/chats", {"agent_id": "casper"})
        c3 = chat3["meta"]["id"]
        ece.post(f"/api/chats/{c3}/send", {"text": "demo:slow"})
        time.sleep(0.8)
        check("job visible while running", any(j["conversation_id"] == c3 and j["running"] for j in ece.get("/api/state")[1]["jobs"]))
        check("viewer does not see private jobs", all(j["conversation_id"] != c3 for j in vera.get("/api/state")[1]["jobs"]))
        ece.post(f"/api/chats/{c3}/stop")
        v = ece.wait_idle(c3, 20)
        check("stopped", v is not None and any("was stopped" in m["text"] for m in v["messages"]), v and v["messages"])
        ece.post(f"/api/chats/{c3}/send", {"text": "demo:fail"})
        v = ece.wait_idle(c3, 20)
        check("failure explained", any("could not finish" in m["text"] for m in v["messages"]))
        check("failed run can be tried again", v["can_retry"] is True, v.get("can_retry"))
        users_before = sum(1 for m in v["messages"] if m["from"].startswith("user:"))
        st, r = ece.post(f"/api/chats/{c3}/retry")
        check("retry started", st == 200, r)
        v = ece.wait_idle(c3, 20)
        check("retry sends the last message again", v is not None and any(m["text"] == "↻ Trying again" for m in v["messages"])
              and sum(1 for m in v["messages"] if m["from"].startswith("user:")) == users_before
              and sum(1 for m in v["messages"] if "could not finish" in m["text"]) == 2, v and v["messages"][-4:])
        ece.post(f"/api/chats/{c3}/send", {"text": "hello again"})
        v = ece.wait_idle(c3, 20)
        check("no retry after an answer", v is not None and v["can_retry"] is False and ece.post(f"/api/chats/{c3}/retry")[0] == 409)
        st, ab = ece.get("/api/about")
        check("editor sees the version but not the update setting", st == 200 and "version" in ab and "check_updates" not in ab, ab)

        # ------------------------------------------- a friend's program with other worker messages is refused
        st, wt = ece.post("/api/worker/token")
        st, r = Client().post("/api/worker/poll", {"providers": ["claude"], "protocol": 999, "version": "9.9.9"},
                              headers={"Authorization": "Bearer " + wt["token"]})
        check("worker with another protocol refused", st == 426 and r.get("version") == ab["version"] and r.get("protocol") == 1, (st, r))
        ece.delete("/api/worker/token")

        # ------------------------------------------- agents act with the asker's rights
        st, adm_only = owner.post("/api/agents", {"name": "Admin Only", "provider": "demo", "skills": ["agent_management"]})
        shared = os.path.join(vault, "Shared_Files")
        before = set(os.listdir(shared))
        st, c4 = ece.post("/api/chats", {"agent_id": "admin_only"})
        ece.post(f"/api/chats/{c4['meta']['id']}/send", {"text": "demo:write"})
        ece.wait_idle(c4["meta"]["id"])
        check("editor: owner-only skill is not effective (no write tool)", set(os.listdir(shared)) == before)
        st, c5 = owner.post("/api/chats", {"agent_id": "admin_only"})
        owner.post(f"/api/chats/{c5['meta']['id']}/send", {"text": "demo:write"})
        owner.wait_idle(c5["meta"]["id"])
        check("owner: same agent has the tools", len(set(os.listdir(shared)) - before) == 1)
        runs = owner.get("/api/runs")[1]["runs"]
        check("runs list for owner", len(runs) >= 6)

        # ----------------------------------------------------------- projects
        st, pr = ece.post("/api/projects", {"name": "Moon Base", "summary": "Build a base on the moon."})
        check("project created", st == 201 and pr["id"] == "moon_base", pr)
        check("project metadata hidden folder", os.path.exists(os.path.join(vault, "Projects", "moon_base", ".Phantom", "project.md")))
        st, note = ece.post("/api/projects/moon_base/notes", {"name": "Oxygen", "body": "We need oxygen."})
        check("note created", st == 201 and note["id"] == "oxygen", note)
        st, chat = ece.post("/api/chats", {"agent_id": "casper", "project_id": "moon_base"})
        ece.post(f"/api/chats/{chat['meta']['id']}/send", {"text": "build demo:write"})
        ece.wait_idle(chat["meta"]["id"])
        check("project run wrote into the project folder", any(f.startswith("demo_") for f in os.listdir(os.path.join(vault, "Projects", "moon_base"))))
        st, fl = ece.get("/api/files?scope=project:moon_base")
        names = [f["path"] for f in fl["files"]]
        check("project files listed without .Phantom", any(n.startswith("demo_") for n in names) and not any(".Phantom" in n for n in names), names)
        check("project file readable", ece.get("/api/files/read?scope=project:moon_base&path=" + names[0])[0] == 200)
        for evil in ["../vault.json", "..%5Cvault.json", "C:/Windows/win.ini", ".Phantom/project.md"]:
            check("traversal blocked: " + evil, ece.get("/api/files/read?scope=project:moon_base&path=" + evil)[0] == 404)
        check("viewer cannot add notes", vera.post("/api/projects/moon_base/notes", {"name": "x"})[0] == 403)
        check("editor cannot delete project", ece.delete("/api/projects/moon_base")[0] == 403)

        # ------------------------------------------- project skills and ghost jobs
        def ask(client, agent, project, text):  # one demo chat, returns the agent's answer
            st, ch = client.post("/api/chats", {"agent_id": agent, "project_id": project})
            client.post(f"/api/chats/{ch['meta']['id']}/send", {"text": text})
            client.wait_idle(ch["meta"]["id"])
            msgs = client.get("/api/chats/" + ch["meta"]["id"])[1]["messages"]
            return "\n".join(m["text"] for m in msgs if m["from"] == "agent:" + agent)
        st, ms = owner.post("/api/skills", {"name": "Moon writing", "tools": ["Write"], "body": "Write the moon diary.", "projects": ["moon_base", "no_such_project"]})
        check("project skill created, unknown project dropped", st == 201 and ms["projects"] == ["moon_base"], ms)
        check("project skill file lists the project", "projects: moon_base" in open(os.path.join(vault, "Skills", "moon_writing.md"), encoding="utf-8").read())
        st, scribe = owner.post("/api/agents", {"name": "Scribe", "role": "Writer", "provider": "demo", "skills": ["read_files", "moon_writing"]})
        check("scribe hired", st == 201, scribe)
        st, p2 = ece.put("/api/projects/moon_base/roles/scribe", {"text": "Writes the moon diary.\nOne page a day."})
        check("ghost job set", st == 200 and p2["roles"].get("scribe", "").startswith("Writes the moon diary."), p2)
        check("ghost job is a file", os.path.exists(os.path.join(vault, "Projects", "moon_base", ".Phantom", "Roles", "scribe.md")))
        check("job for unknown ghost refused", ece.put("/api/projects/moon_base/roles/nobody", {"text": "x"})[0] == 404)
        check("viewer cannot set ghost jobs", vera.put("/api/projects/moon_base/roles/scribe", {"text": "x"})[0] == 403)
        before = len(os.listdir(os.path.join(vault, "Projects", "moon_base")))
        ans = ask(ece, "scribe", "moon_base", "demo:write demo:prompt")
        check("project skill works in its project", len(os.listdir(os.path.join(vault, "Projects", "moon_base"))) == before + 1, ans[:300])
        check("prompt has the project skill", "### Moon writing" in ans, ans)
        check("prompt has the ghost job", "## Your job in this project\nWrites the moon diary.\nOne page a day." in ans, ans)
        shared_before = len(os.listdir(os.path.join(vault, "Shared_Files")))
        ans = ask(ece, "scribe", "", "demo:write demo:prompt")
        check("project skill left out elsewhere", len(os.listdir(os.path.join(vault, "Shared_Files"))) == shared_before and "Moon writing" not in ans, ans)
        check("ghost job left out elsewhere", "Your job in this project" not in ans, ans)
        ans = ask(ece, "casper", "moon_base", "demo:prompt")
        check("colleagues see the first line of a job", "Scribe, Writer; in this project: Writes the moon diary.\n" in ans, ans)
        check("ghost job removed", ece.put("/api/projects/moon_base/roles/scribe", {"text": " "})[1]["roles"] == {}
              and not os.path.exists(os.path.join(vault, "Projects", "moon_base", ".Phantom", "Roles", "scribe.md")))

        # ---------------------------------------------------------------- tasks
        st, t = ece.post("/api/tasks", {"title": "Write a plan", "description": "Plan the first milestone. demo:write", "agent_id": "casper", "project_id": "moon_base"})
        check("task created", st == 201 and t["conversation_id"], t)
        t0 = time.time()
        while time.time() - t0 < 60:
            if ece.get("/api/tasks/" + t["id"])[1]["task"]["status"] == "done":
                break
            time.sleep(0.3)
        d = ece.get("/api/tasks/" + t["id"])[1]
        check("task finished", d["task"]["status"] == "done" and len(d["runs"]) == 1, d["task"])
        check("task visible to the whole team", vera.get("/api/tasks/" + t["id"])[0] == 200)
        conv = vera.get("/api/chats/" + t["conversation_id"])[1]
        check("task conversation shows result", any(m["from"] == "agent:casper" for m in conv["messages"]))
        check("task is a markdown file", os.path.exists(os.path.join(vault, "Tasks", t["id"] + ".md")))
        st, r = ece.post(f"/api/tasks/{t['id']}/run", {"note": "once more"})
        check("task rerun with note", st == 201)
        ece.wait_idle(t["conversation_id"])
        check("rerun finished", len(ece.get("/api/tasks/" + t["id"])[1]["runs"]) == 2)
        check("only creator or owner delete task", vera.delete("/api/tasks/" + t["id"])[0] == 403)

        # -------------------------------------------------------- task templates
        st, tp = ece.post("/api/task_templates", {"name": "Moon report", "description": "Report the day.", "agent_id": "casper", "project_id": "moon_base", "wake": "daily", "at": "09:30"})
        check("template created", st == 201 and tp["id"] == "moon_report" and tp["wake"] == "daily" and tp["wake_at"] == "09:30", tp)
        check("template is a markdown file", os.path.exists(os.path.join(vault, "Task_Templates", "moon_report.md")))
        check("template bad time refused", ece.post("/api/task_templates", {"name": "X", "wake": "daily", "at": "25:00"})[0] == 400)
        check("template unknown ghost refused", ece.post("/api/task_templates", {"name": "X", "agent_id": "nobody"})[0] == 400)
        check("viewer cannot make templates", vera.post("/api/task_templates", {"name": "X"})[0] == 403)
        check("viewer can list templates", [x["id"] for x in vera.get("/api/task_templates")[1]["templates"]] == ["moon_report"])
        st, otp = owner.post("/api/task_templates", {"name": "Owner only"})
        check("only maker or owner edits a template", ece.put("/api/task_templates/" + otp["id"], {"name": "Mine"})[0] == 403
              and owner.put("/api/task_templates/moon_report", {"name": "Moon report daily"})[1]["name"] == "Moon report daily")
        st, tt = ece.post("/api/tasks", {"title": tp["name"], "description": tp["description"], "agent_id": "casper", "project_id": "moon_base",
                                         "run": False, "wake": "daily", "every_min": 0, "at": "09:30"})
        check("task from template keeps the wake-up", st == 201 and tt["wake"] == "daily" and tt["wake_at"] == "09:30" and tt["wake_on"], tt)
        check("task with bad wake-up refused", ece.post("/api/tasks", {"title": "X", "agent_id": "casper", "run": False, "wake": "interval", "every_min": 0})[0] == 400)
        ece.delete("/api/tasks/" + tt["id"])
        check("template deleted", ece.delete("/api/task_templates/moon_report")[0] == 200 and owner.delete("/api/task_templates/" + otp["id"])[0] == 200
              and ece.get("/api/task_templates")[1]["templates"] == [])

        # removing a project removes its tasks (and their chats), not other tasks
        st, tmp_p = ece.post("/api/projects", {"name": "Throwaway"})
        _, pt = ece.post("/api/tasks", {"title": "Doomed", "agent_id": "casper", "project_id": tmp_p["id"], "run": False})
        _, ot = ece.post("/api/tasks", {"title": "Survivor", "agent_id": "casper", "run": False})
        st, r = owner.delete("/api/projects/" + tmp_p["id"])
        check("project removal reports its tasks", st == 200 and r.get("tasks_removed") == 1, r)
        check("project's task is gone", ece.get("/api/tasks/" + pt["id"])[0] == 404
              and not os.path.exists(os.path.join(vault, "Conversations", pt["conversation_id"] + ".jsonl")))
        check("other tasks stay", ece.get("/api/tasks/" + ot["id"])[0] == 200)
        ece.delete("/api/tasks/" + ot["id"])

        # ----------------------------------------------------- skills and rules
        st, s = owner.post("/api/skills", {"name": "Shell stuff", "tools": ["Bash", "Read"], "body": "Be careful.", "min_role": "editor"})
        check("shell skill forced to owner-only", st == 201 and s["min_role"] == "owner", s)
        st, tl = ece.get("/api/skill_templates")
        check("templates listed", st == 200 and any(x["id"] == "web_research" for x in tl["templates"]))
        check("template installed by owner", owner.post("/api/skills/from_template", {"id": "web_research"})[0] == 201)
        check("rule created by owner", owner.post("/api/rules", {"name": "Be nice", "body": "Always be kind."})[0] == 201)
        check("editor cannot create rules", ece.post("/api/rules", {"name": "x", "body": "y"})[0] == 403)

        # ------------------------------------------------------- password, delete
        check("wrong old password refused", ece.put("/api/me", {"password": "new password 12", "old_password": "bad"})[0] == 403)
        check("password changed", ece.put("/api/me", {"password": "new password 12", "old_password": "ece password 1"})[0] == 200)
        check("old password no longer works", Client().post("/api/auth/login", {"name": "ece", "password": "ece password 1"})[0] == 401)
        check("new password works", Client().post("/api/auth/login", {"name": "ece", "password": "new password 12"})[0] == 200)
        check("settings updated", owner.put("/api/settings", {"max_hops": 3, "default_model": "haiku"})[1]["max_hops"] == 3)
        check("bad model refused", owner.put("/api/settings", {"default_model": "x; calc"})[0] == 400)
        check("delete user", owner.delete("/api/users/vera")[0] == 200 and vera.get("/api/state")[0] == 401)

        # ---------------------------------------------------------- network switch
        st, n = owner.get("/api/network")
        check("network info", st == 200 and n["share"] is False and n["port"] == PORT)
        st, n = owner.put("/api/network", {"share": True})
        check("sharing switched on", st == 200 and n["share"] is True)
        time.sleep(1.5)
        st, _ = owner.get("/api/state")
        check("server restarted and session kept", st == 200, st)
        check("foreign Host allowed only when sharing", anon.get("/api/auth/status", headers={"Host": "192.168.1.5:7791"})[0] == 200)
        check("api still needs sign-in when sharing", anon.get("/api/state", headers={"Host": "192.168.1.5:7791"})[0] == 401)
        owner.put("/api/network", {"share": False})
        time.sleep(1.5)
        check("sharing switched off", anon.get("/api/auth/status", headers={"Host": "192.168.1.5:7791"})[0] == 403)

        # ----------------------------------------- restart keeps everything on disk
        stop(proc)
        proc = start(exe, vault, data)
        st, _ = owner.get("/api/state")
        check("session survives restart", st == 200, st)
        check("chats survive restart", owner.get("/api/chats/" + cid)[0] == 200)

        if real:
            st, ra = owner.post("/api/agents", {"name": "Real", "role": "Tester", "provider": real, "skills": ["read_files", "write_files"], "model": "haiku" if real == "claude" else ""})
            st, ch = owner.post("/api/chats", {"agent_id": ra["id"]})
            rid = ch["meta"]["id"]
            owner.post(f"/api/chats/{rid}/send", {"text": "Create a file named real_hello.txt containing the word hello, then answer with one short sentence."})
            v = owner.wait_idle(rid, 300)
            print("real messages:", [(m["from"], m["text"][:80]) for m in v["messages"]])
            check(f"real {real} answered", any(m["from"] == "agent:" + ra["id"] for m in v["messages"]))
            check(f"real {real} created file", os.path.exists(os.path.join(vault, "Shared_Files", "real_hello.txt")))
            check(f"real {real} session stored", bool(open(os.path.join(vault, "Conversations", rid + ".jsonl"), encoding="utf-8").read().count('"t":"session"')))
            owner.post(f"/api/chats/{rid}/send", {"text": "What was the file name I asked for? Answer in 3 words."})
            v = owner.wait_idle(rid, 300)
            last = [m for m in v["messages"] if m["from"].startswith("agent:")][-1]["text"]
            print("resumed answer:", last)
            check(f"real {real} remembers (session resume)", "real_hello" in last.lower() or "hello" in last.lower(), last)
            run = [r for r in owner.get("/api/runs")[1]["runs"] if r["agent_id"] == ra["id"]][0]
            print("run record:", run)
    finally:
        stop(proc)

    # ------------------------------------------------ upgrade of a version 1 vault
    vault1, data1 = os.path.join(tmp, "Vault1"), os.path.join(tmp, "Data1")
    os.makedirs(os.path.join(vault1, "Agents"))
    os.makedirs(os.path.join(vault1, "Tasks", "T1"))
    json.dump({"vault_version": 1, "vault_id": "X", "created": "2026-01-01T00:00:00Z"}, open(os.path.join(vault1, "vault.json"), "w"))
    json.dump({"id": "luna_74JGC5", "name": "Luna", "role": "assistant", "provider": "claude", "model": "haiku", "form": "cat", "color": "#ffe27a",
               "access": "workspace_shell", "prompt": "Be nice.", "timeout_min": 30, "created": "2026-01-01T00:00:00Z"},
              open(os.path.join(vault1, "Agents", "luna_74JGC5.json"), "w"))
    proc = start(exe, vault1, data1)
    try:
        c = Client()
        c.post("/api/auth/setup", {"name": "Meto", "password": "correct horse 1"})
        st, state = c.get("/api/state")
        luna = [a for a in state["agents"] if a["id"] == "luna_74JGC5"]
        check("v1 agent converted", len(luna) == 1 and luna[0]["name"] == "Luna" and luna[0]["model"] == "haiku" and luna[0]["persona"] == "Be nice.", luna)
        check("v1 access became skills", luna and luna[0]["skills"] == ["read_files", "write_files", "shell_commands"], luna)
        check("needed skills installed", {"read_files", "write_files", "shell_commands"} <= {s["id"] for s in state["skills"]})
        check("v1 files archived", os.path.exists(os.path.join(vault1, "Archive_V1", "Agents", "luna_74JGC5.json")) and os.path.exists(os.path.join(vault1, "Archive_V1", "Tasks", "T1")))
        check("vault is version 2", json.load(open(os.path.join(vault1, "vault.json")))["vault_version"] == 2)
    finally:
        stop(proc)
        shutil.rmtree(tmp, ignore_errors=True)

    print("\nALL PASSED" if not failures else f"\n{len(failures)} FAILED: {failures}")
    sys.exit(1 if failures else 0)


main()
