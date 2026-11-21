"""MCP server for the person's own AI assistant (POST /mcp with an API key): demo ghosts only, no internet.

Usage: python -X utf8 -I assistant_mcp_test.py <absolute path of phantom_co.exe>
"""
import json
import os
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

PORT = 7799
BASE = f"http://127.0.0.1:{PORT}"
failures = []


def check(name, cond, extra=""):
    print(("PASS " if cond else "FAIL ") + name + (f"  [{extra}]" if extra and not cond else ""))
    if not cond:
        failures.append(name)


class Client:
    def __init__(self):
        self.cookie = ""

    def call(self, method, path, body=None):
        data = json.dumps(body).encode() if body is not None else None
        h = {"X-PC2": "1", "Content-Type": "application/json"}
        if self.cookie:
            h["Cookie"] = self.cookie
        req = urllib.request.Request(BASE + path, data=data, method=method, headers=h)
        try:
            with urllib.request.urlopen(req, timeout=20) as r:
                sc = r.headers.get("Set-Cookie")
                if sc and "pc2_session_" in sc:
                    self.cookie = f"pc2_session_{PORT}=" + sc.split(f"pc2_session_{PORT}=")[1].split(";")[0]
                return r.status, json.loads(r.read())
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read() or b"{}")


def raw_mcp(key, body, method="POST"):
    h = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream"}
    if key:
        h["Authorization"] = "Bearer " + key
    data = (body if isinstance(body, bytes) else json.dumps(body).encode()) if body is not None else None
    req = urllib.request.Request(BASE + "/mcp", data=data, method=method, headers=h)
    try:
        with urllib.request.urlopen(req, timeout=120) as r:
            text = r.read()
            return r.status, json.loads(text) if text else None
    except urllib.error.HTTPError as e:
        text = e.read()
        return e.code, json.loads(text) if text else None


_next = [0]


def rpc(key, method, params=None):
    _next[0] += 1
    st, r = raw_mcp(key, {"jsonrpc": "2.0", "id": _next[0], "method": method, "params": params or {}})
    return r


def tool(key, tool_name, **args):
    """(text, is_error) of one tool call."""
    r = rpc(key, "tools/call", {"name": tool_name, "arguments": args})
    if not r or "result" not in r:
        return f"<no result: {r}>", True
    return r["result"]["content"][0]["text"], r["result"]["isError"]


def field(text, name):
    for line in text.splitlines():
        if line.startswith(name + ": "):
            return line[len(name) + 2:].strip()
    return ""


def main():
    exe = sys.argv[1]
    tmp = tempfile.mkdtemp(prefix="pc2_amcp_")
    proc = subprocess.Popen([exe, "--vault", os.path.join(tmp, "Vault"), "--data", os.path.join(tmp, "Data"), "--port", str(PORT), "--no-window"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        o, vera = Client(), Client()
        for _ in range(50):
            try:
                o.call("GET", "/api/auth/status")
                break
            except Exception:
                time.sleep(0.2)
        check("setup", o.call("POST", "/api/auth/setup", {"name": "Meto", "password": "correct horse 1"})[0] == 200)
        st, r = o.call("POST", "/api/users", {"name": "Vera", "role": "viewer"})
        vera.call("POST", "/api/auth/join", {"token": r["invite_token"], "password": "vera password 1"})
        st, a = o.call("POST", "/api/agents", {"name": "Casper", "provider": "demo", "role": "Writes short answers"})
        check("demo ghost", st == 201, a)
        gid = a.get("id", "")
        key = o.call("POST", "/api/api_keys", {"name": "assistant"})[1]["token"]
        vkey = vera.call("POST", "/api/api_keys", {"name": "viewer assistant"})[1]["token"]

        # transport
        st, r = raw_mcp(None, {"jsonrpc": "2.0", "id": 1, "method": "ping"})
        check("no key: 401", st == 401, (st, r))
        st, r = Client().call("POST", "/mcp", {"jsonrpc": "2.0", "id": 1, "method": "ping"})
        check("a browser session cannot use /mcp", st == 401, (st, r))
        check("GET /mcp: 405", raw_mcp(key, None, "GET")[0] == 405)
        st, r = raw_mcp(key, b"{not json")
        check("bad JSON: parse error", st == 400 and r["error"]["code"] == -32700, r)
        r = rpc(key, "initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "test", "version": "1"}})
        res = (r or {}).get("result", {})
        check("initialize", res.get("protocolVersion") == "2025-06-18" and res.get("serverInfo", {}).get("name") == "phantom-co"
              and "tools" in res.get("capabilities", {}) and "list_ghosts" in res.get("instructions", ""), r)
        check("an unknown protocol version gets ours", rpc(key, "initialize", {"protocolVersion": "1999-01-01"})["result"]["protocolVersion"] == "2025-06-18")
        st, r = raw_mcp(key, {"jsonrpc": "2.0", "method": "notifications/initialized"})
        check("a notification: 202 without body", st == 202 and r is None, (st, r))
        check("ping", rpc(key, "ping").get("result") == {})
        r = rpc(key, "tools/list")
        names = [t["name"] for t in r["result"]["tools"]]
        check("tools/list", {"list_ghosts", "ask_ghost", "get_reply", "create_task", "save_script", "run_script", "get_script_run", "search"} <= set(names), names)
        check("every tool has a schema", all(t["inputSchema"]["type"] == "object" for t in r["result"]["tools"]))
        check("unknown method", rpc(key, "nope")["error"]["code"] == -32601)
        check("unknown tool", rpc(key, "tools/call", {"name": "nope", "arguments": {}})["error"]["code"] == -32602)
        st, r = raw_mcp(key, [{"jsonrpc": "2.0", "id": 90, "method": "ping"}, {"jsonrpc": "2.0", "method": "notifications/x"}, {"jsonrpc": "2.0", "id": 91, "method": "ping"}])
        check("a batch", st == 200 and [x["id"] for x in r] == [90, 91], r)

        # ghosts and chats
        text, err = tool(key, "list_ghosts")
        check("list_ghosts", not err and "Casper" in text and gid in text and "Writes short answers" in text, text)
        text, err = tool(key, "ask_ghost", ghost="casper", message="hello there", wait_seconds=20)
        chat = field(text, "chat_id")
        check("ask_ghost by name (any case) waits for the answer", not err and chat and field(text, "status") == "done" and "Casper (ghost)" in text, text)
        text, err = tool(key, "ask_ghost", chat_id=chat, message="and again", wait_seconds=20)
        check("ask_ghost goes on in the same chat", not err and field(text, "chat_id") == chat and field(text, "status") == "done", text)
        text, err = tool(key, "ask_ghost", ghost=gid, message="demo:slow take your time", wait_seconds=1)
        slow = field(text, "chat_id")
        check("a slow ghost: still working", not err and field(text, "status") == "still working" and "get_reply" in text, text)
        text, err = tool(key, "get_reply", chat_id=slow, wait_seconds=30)
        check("get_reply waits for the rest", not err and field(text, "status") == "done" and "Casper (ghost)" in text, text)
        text, err = tool(key, "ask_ghost", ghost="Nobody", message="hi")
        check("an unknown ghost", err and "list_ghosts" in text, text)
        text, err = tool(key, "ask_ghost", message="hi")
        check("no ghost and no chat", err, text)
        text, err = tool(key, "get_reply", chat_id="../api/users")
        check("an id with a path in it is refused", err and "Not a valid id" in text, text)
        text, err = tool(key, "list_chats")
        check("list_chats", not err and chat in text and slow in text, text)
        text, err = tool(key, "read_chat", chat_id=chat, last=2)
        check("read_chat", not err and "and again" in text and "hello there" not in text, text)
        check("stop_work", tool(key, "stop_work", chat_id=chat) == ("Stopped.", False))

        # tasks
        text, err = tool(key, "create_task", title="Daily note", description="write a note", ghost="Casper", wake="interval", every_min=60, wait_seconds=20)
        tid, tchat = field(text, "task_id"), field(text, "chat_id")
        check("create_task with a wake-up and an answer", not err and tid and "every 60 min" in text and field(text, "status") == "done", text)
        text, err = tool(key, "list_tasks")
        check("list_tasks", not err and tid in text and "wakes every 60 min" in text, text)
        text, err = tool(key, "set_task_wake", task_id=tid, wake="daily", at="09:30")
        check("set_task_wake daily", not err and "daily at 09:30" in text, text)
        text, err = tool(key, "set_task_wake", task_id=tid, wake="daily", at="25:00")
        check("a wrong time is explained", err and "HH:MM" in text, text)
        text, err = tool(key, "run_task", task_id=tid, note="once more", wait_seconds=20)
        check("run_task", not err and field(text, "chat_id") == tchat and field(text, "status") == "done", text)
        text, err = tool(key, "create_task", title="Later", ghost="Casper", run=False)
        check("a task that does not start", not err and "task_id" in text, text)

        # scripts
        text, err = tool(key, "script_format")
        check("script_format", not err and "save_script" in text and '"nodes"' in text, text[:200])
        script = {"description": "say hi", "vars": [], "nodes": [{"id": "s", "type": "start", "x": 0, "y": 0},
                  {"id": "p", "type": "prompt", "x": 0, "y": 120, "label": "Hi", "props": {"agent_id": gid, "text": "Say hi"}},
                  {"id": "e", "type": "end", "x": 0, "y": 240}],
                  "edges": [{"from": "s", "to": "p"}, {"from": "p", "to": "e"}]}
        text, err = tool(key, "save_script", name="Hello flow", script=script)
        sid = field(text, "script_id")
        check("save_script", not err and sid and "No problems" in text, text)
        bad = dict(script, nodes=script["nodes"] + [{"id": "q", "type": "prompt", "x": 200, "y": 0, "props": {"agent_id": "nobody_here", "text": "x"}}])
        text, err = tool(key, "save_script", name="Hello flow", script=bad, script_id=sid)
        check("save_script lists problems", not err and field(text, "script_id") == sid and "cannot run" in text, text)
        text, err = tool(key, "run_script", script_id=sid)
        check("a script with problems does not run", err and "problems" in text.lower(), text)
        tool(key, "save_script", name="Hello flow", script=script, script_id=sid)
        text, err = tool(key, "list_scripts")
        check("list_scripts", not err and sid in text and "Hello flow" in text and "[draft]" not in text, text)
        text, err = tool(key, "get_script", script_id=sid)
        check("get_script", not err and json.loads(text)["id"] == sid, text[:200])
        text, err = tool(key, "run_script", script_id=sid)
        run = field(text, "run_id")
        check("run_script", not err and run, text)
        status = ""
        for _ in range(100):
            text, err = tool(key, "get_script_run", run_id=run)
            status = field(text, "status")
            if status not in ("running", "queued", ""):
                break
            time.sleep(0.2)
        check("the script run ends", status == "done" and "p (prompt, Hi): done" in text, text)
        check("save_script needs an object", tool(key, "save_script", name="x", script="nodes")[1])

        # search, rights
        text, err = tool(key, "search", query="hello there")
        check("search", not err and chat in text, text[:300])
        text, err = tool(vkey, "ask_ghost", ghost="Casper", message="hi")
        check("a viewer's assistant cannot chat", err and "role" in text.lower(), text)
        text, err = tool(vkey, "list_ghosts")
        check("a viewer's assistant reads", not err and "Casper" in text, text)
        text, err = tool(vkey, "read_chat", chat_id=chat)
        check("a viewer's assistant does not see the owner's chat", err, text)
    finally:
        proc.terminate()
        proc.wait(10)
    print(f"\n{len(failures)} failed" if failures else "\nall passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
