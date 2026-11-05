"""MCP test: Settings > MCP API, secrets, the proxy, and (with --real) a ghost that calls MCP tools and waits for approval.

Usage: python -X utf8 -I mcp_test.py <absolute path of phantom_co.exe> [--real claude|codex]
Without --real nothing is sent to an AI. With --real a small model calls a local echo server (needs Python here) and the
built-in Telegram server without a token; one call is approved and one declined through the API.
"""
import json
import os
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

PORT = 7793
BASE = f"http://127.0.0.1:{PORT}"
failures = []

# A tiny stdio MCP server without dependencies: echo (free) and shout (asks for approval).
ECHO_SERVER = r'''
import json, os, sys
def out(m):
    sys.stdout.write(json.dumps(m) + "\n"); sys.stdout.flush()
for line in sys.stdin:
    try:
        m = json.loads(line)
    except Exception:
        continue
    if "id" not in m:
        continue
    meth = m.get("method")
    if meth == "initialize":
        out({"jsonrpc": "2.0", "id": m["id"], "result": {"protocolVersion": m["params"].get("protocolVersion", "2025-06-18"),
             "capabilities": {"tools": {}}, "serverInfo": {"name": "echo", "version": "1"}}})
    elif meth == "tools/list":
        schema = {"type": "object", "properties": {"text": {"type": "string"}}, "required": ["text"]}
        out({"jsonrpc": "2.0", "id": m["id"], "result": {"tools": [
            {"name": "echo", "description": "Echo a text back.", "inputSchema": schema},
            {"name": "shout", "description": "Shout a text (upper case).", "inputSchema": schema}]}})
    elif meth == "tools/call":
        p = m["params"]; t = p.get("arguments", {}).get("text", "")
        ok = "secret=ok" if os.environ.get("ECHO_SECRET") == "s3cret" else "secret=missing"
        txt = ("echo: " + t if p["name"] == "echo" else "shout: " + t.upper()) + " (" + ok + ")"
        out({"jsonrpc": "2.0", "id": m["id"], "result": {"content": [{"type": "text", "text": txt}]}})
    else:
        out({"jsonrpc": "2.0", "id": m["id"], "result": {}})
'''


def check(name, cond, extra=""):
    print(("PASS " if cond else "FAIL ") + name + (f"  [{extra}]" if extra and not cond else ""))
    if not cond:
        failures.append(name)


class Client:
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
            with urllib.request.urlopen(req, timeout=40) as r:
                raw = r.read()
                sc = r.headers.get("Set-Cookie")
                if sc and "pc2_session_" in sc:
                    self.cookie = f"pc2_session_{PORT}=" + sc.split(f"pc2_session_{PORT}=")[1].split(";")[0]
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


def wait(fn, timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        v = fn()
        if v:
            return v
        time.sleep(0.5)
    return None


def main():
    exe = sys.argv[1]
    real = sys.argv[sys.argv.index("--real") + 1] if "--real" in sys.argv else None
    tmp = tempfile.mkdtemp(prefix="pc2_mcp_")
    vault, data, srv = os.path.join(tmp, "Vault"), os.path.join(tmp, "Data"), os.path.join(tmp, "Echo_Server")
    os.makedirs(srv)
    with open(os.path.join(srv, "server.py"), "w", encoding="utf-8") as f:
        f.write(ECHO_SERVER)
    proc = subprocess.Popen([exe, "--vault", vault, "--data", data, "--port", str(PORT), "--no-window"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        owner = Client()
        wait(lambda: owner.get("/api/auth/status")[0] == 200 if True else None, 15)
        st, _ = owner.post("/api/auth/setup", {"name": "Meto", "password": "correct horse 1"})
        check("setup", st == 200)

        # ---------------------------------------------------------- the list
        st, r = owner.post("/api/mcp", {"catalog": "telegram"})
        check("ready-made telegram added", st == 201 and r["name"] == "telegram" and r.get("skill_id") == "telegram", r)
        sk = open(os.path.join(vault, "Skills", "telegram.md"), encoding="utf-8").read()
        check("its skill opens the server", "mcp: telegram" in sk, sk[:200])
        st, r = owner.post("/api/mcp", {"catalog": "telegram"})
        check("added twice refused", st == 400, r)
        st, r = owner.post("/api/mcp", {"name": "Bad Name", "kind": "command", "command": "python"})
        check("bad name refused", st == 400, r)
        st, r = owner.post("/api/mcp", {"name": "x", "kind": "command", "command": 'py"thon'})
        check("quote in command refused", st == 400, r)
        st, r = owner.post("/api/mcp", {"name": "echo", "kind": "command", "command": sys.executable, "args": ["server.py"], "folder": srv,
                                        "env_names": ["ECHO_SECRET"], "secrets": {"ECHO_SECRET": "s3cret"}, "approve": True, "approve_tools": ["shout"]})
        check("own server added", st == 201, r)
        check("its folder was copied out of the Vault", os.path.exists(os.path.join(data, "Mcp", "echo", "server.py")))
        st, lst = owner.get("/api/mcp")
        echo = next((s for s in lst["servers"] if s["name"] == "echo"), {})
        check("list shows it found and its secret set", echo.get("found") and echo.get("secrets_set") == ["ECHO_SECRET"], echo)
        raw = json.dumps(lst) + open(os.path.join(data, "mcp.json"), encoding="utf-8").read() + open(os.path.join(data, "secrets.json"), encoding="utf-8").read()
        check("secret value never shown or stored in plain text", "s3cret" not in raw)
        check("nothing about MCP in the Vault", not any("s3cret" in open(os.path.join(dp, f), encoding="utf-8", errors="ignore").read()
                                                        for dp, _, fs in os.walk(vault) for f in fs if f != ".phantom_lock"))
        st, r = owner.put("/api/mcp/echo", {"secrets": {"OTHER": "x"}})
        check("unknown secret name refused", st == 400, r)
        st, r = owner.put("/api/mcp/echo", {"label": "Echo"})
        check("change keeps the secret", st == 200 and owner.get("/api/mcp")[1]["servers"][1]["secrets_set"] == ["ECHO_SECRET"], r)

        # ---------------------------------------------------------- skills
        st, s = owner.post("/api/skills", {"name": "Echo test", "description": "x", "tools": [], "mcp": ["echo", "telegram", "nope"],
                                           "body": "Use the echo server and Telegram when asked."})
        check("skill keeps only existing servers", st == 201 and s.get("mcp") == ["echo", "telegram"], s)
        st, state = owner.get("/api/state")
        check("state lists the servers", [m["name"] for m in state.get("mcp", [])] == ["telegram", "echo"], state.get("mcp"))

        # ---------------------------------------------------------- the proxy side
        st, r = Client().post("/api/mcp/session", {"key": "bad", "server": "echo"})
        check("session needs a run key", st == 403, r)
        st, r = Client().post("/api/mcp/ask", {"key": "bad", "server": "echo", "tool": "shout"})
        check("ask with a bad key is cancelled", st == 200 and r.get("status") == "cancelled", r)
        p = subprocess.run([exe, "--mcp-proxy", "echo", "--mcp-port", str(PORT), "--mcp-key", "bad"], input=b"", capture_output=True, timeout=30)
        check("proxy with a bad key ends at once and writes nothing to stdout", p.returncode == 1 and p.stdout == b"", (p.returncode, p.stdout[:200], p.stderr[:200]))

        if real:
            agent = {"name": "Tester", "role": "Tester", "provider": real, "model": "haiku" if real == "claude" else "", "skills": [s["id"]],
                     "persona": "You follow instructions exactly and quote tool results word for word."}
            st, a = owner.post("/api/agents", agent)
            check("agent created", st == 201, a)
            st, chat = owner.post("/api/chats", {"agent_id": a["id"]})
            cid = chat["meta"]["id"]
            owner.post(f"/api/chats/{cid}/send", {"text": "Call mcp__echo__echo with text 'pumpkin', then mcp__echo__shout with text 'boo', then "
                                                          "mcp__telegram__read_messages. Then answer with the three tool results, word for word, one per line."})

            def pending():
                v = owner.get(f"/api/chats/{cid}")[1]
                return v if v.get("approvals") else (v if not v["busy"] else None)
            v = wait(pending, 240)
            check("the shout call waits for approval", v and v.get("approvals"), v and v.get("messages")[-3:])
            if v and v.get("approvals"):
                ap = v["approvals"][0]
                msg = next((m for m in v["messages"] if m.get("approval") == ap["id"]), {})
                check("approval card in the chat", msg.get("kind") == "approval" and msg.get("tool") == "shout" and "boo" in msg.get("args", ""), msg)
                st, sv = owner.get("/api/state")
                check("state shows it to the owner", any(x["id"] == ap["id"] and x["can_decide"] for x in sv.get("approvals", [])))
                check("approve", owner.post(f"/api/mcp/approvals/{ap['id']}", {"approve": True})[0] == 200)
                check("second answer refused", owner.post(f"/api/mcp/approvals/{ap['id']}", {"approve": False})[0] == 409)
            v = wait(lambda: (lambda x: x if not x["busy"] else None)(owner.get(f"/api/chats/{cid}")[1]), 300)
            ans = next((m["text"] for m in reversed(v["messages"]) if m["from"].startswith("agent:")), "") if v else ""
            check("echo result came back with its secret", "echo: pumpkin (secret=ok)" in ans, ans)
            check("approved shout ran", "BOO" in ans, ans)
            check("built-in telegram answered (no token)", "no token" in ans.lower() or "token" in ans.lower(), ans)

            owner.post(f"/api/chats/{cid}/send", {"text": "Call mcp__echo__shout with text 'again' once and tell me what happened."})
            v = wait(pending, 240)
            if v and v.get("approvals"):
                check("decline", owner.post(f"/api/mcp/approvals/{v['approvals'][0]['id']}", {"approve": False})[0] == 200)
            else:
                check("second call waits for approval", False, v and v.get("messages")[-3:])
            v = wait(lambda: (lambda x: x if not x["busy"] else None)(owner.get(f"/api/chats/{cid}")[1]), 300)
            texts = [m["text"] for m in v["messages"]] if v else []
            check("declined line in the chat", any(t.startswith("✗ Meto declined") for t in texts), texts[-4:])
            check("nothing shouted the second time", not any("AGAIN" in t and "shout:" in t for t in texts), texts[-3:])
            leftovers = [f for f in os.listdir(os.path.join(os.environ["LOCALAPPDATA"], "Phantom_Co", "Tmp")) if f.endswith(".mcp.json")]
            check("run's MCP file removed", not leftovers, leftovers)
    finally:
        proc.terminate()
        try:
            proc.wait(5)
        except Exception:
            proc.kill()
    print("\nALL PASSED" if not failures else f"\n{len(failures)} FAILED: {failures}")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
