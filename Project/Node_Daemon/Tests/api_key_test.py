"""API keys test: keys for the person's own programs (an AI assistant), no internet, demo ghosts only.

Usage: python -X utf8 -I api_key_test.py <absolute path of phantom_co.exe>
"""
import json
import os
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

PORT = 7798
BASE = f"http://127.0.0.1:{PORT}"
failures = []


def check(name, cond, extra=""):
    print(("PASS " if cond else "FAIL ") + name + (f"  [{extra}]" if extra and not cond else ""))
    if not cond:
        failures.append(name)


class Client:
    """A browser session (cookie + X-PC2)."""

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


def key_call(key, method, path, body=None, origin=None):
    """A program with an API key: no cookie, no X-PC2."""
    data = json.dumps(body).encode() if body is not None else None
    h = {"Authorization": "Bearer " + key, "Content-Type": "application/json"}
    if origin:
        h["Origin"] = origin
    req = urllib.request.Request(BASE + path, data=data, method=method, headers=h)
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"{}")


def wait_quiet(key, chat_id, secs=20):
    for _ in range(secs * 5):
        st, v = key_call(key, "GET", "/api/chats/" + chat_id)
        if st == 200 and not v["busy"]:
            return v
        time.sleep(0.2)
    return None


def main():
    exe = sys.argv[1]
    tmp = tempfile.mkdtemp(prefix="pc2_key_")
    vault, data = os.path.join(tmp, "Vault"), os.path.join(tmp, "Data")
    proc = subprocess.Popen([exe, "--vault", vault, "--data", data, "--port", str(PORT), "--no-window"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        o, ece, vera = Client(), Client(), Client()
        for _ in range(50):
            try:
                o.call("GET", "/api/auth/status")
                break
            except Exception:
                time.sleep(0.2)
        check("setup", o.call("POST", "/api/auth/setup", {"name": "Meto", "password": "correct horse 1"})[0] == 200)
        for who, name, role in ((ece, "Ece", "editor"), (vera, "Vera", "viewer")):
            st, r = o.call("POST", "/api/users", {"name": name, "role": role})
            who.call("POST", "/api/auth/join", {"token": r["invite_token"], "password": name.lower() + " password 1"})
        st, a = o.call("POST", "/api/agents", {"name": "Casper", "provider": "demo"})
        check("demo ghost", st == 201, a)

        # making keys
        check("a key needs a name", o.call("POST", "/api/api_keys", {"name": " "})[0] == 400)
        st, r = o.call("POST", "/api/api_keys", {"name": "My assistant"})
        key = r.get("token", "")
        check("the owner makes a key", st == 201 and key.startswith("pc_") and len(key) == 51 and r["key"]["hint"] == "…" + key[-4:], r)
        st, r = o.call("GET", "/api/api_keys")
        check("the list shows it without the secret", st == 200 and len(r["keys"]) == 1 and r["keys"][0]["name"] == "My assistant"
              and "token" not in r["keys"][0] and r["keys"][0]["used"] == "", r)
        auth_text = open(os.path.join(data, "auth.json"), encoding="utf-8").read()
        check("auth.json holds only the hash", key not in auth_text and "api_keys" in auth_text)

        # using a key
        st, s = key_call(key, "GET", "/api/state")
        check("a key reads the state without cookie or X-PC2", st == 200 and any(x["id"] == a["id"] for x in s["agents"]), st)
        check("the key shows when it was used", o.call("GET", "/api/api_keys")[1]["keys"][0]["used"] != "")
        st, c = key_call(key, "POST", "/api/chats", {"agent_id": a["id"]}, origin="https://evil.example")
        check("a key starts a chat (no Origin check)", st == 201, c)
        cid = c.get("meta", {}).get("id", "")
        st, v = key_call(key, "POST", f"/api/chats/{cid}/send", {"text": "hello from my assistant"})
        check("a key sends a message", st == 200, v)
        v = wait_quiet(key, cid)
        check("the ghost answers", v is not None and any(m.get("from", "").startswith("agent:") for m in v["messages"]), v)
        check("the message is the key owner's", v is not None and any(m.get("from") == "user:meto" for m in v["messages"]))
        st, t = key_call(key, "POST", "/api/tasks", {"title": "Key task", "description": "do it", "agent_id": a["id"]})
        check("a key makes and runs a task", st == 201 and t.get("created_by") == "meto", t)
        st, p = o.call("POST", "/api/projects", {"name": "Keyproj"})
        check("a project to delete", st in (200, 201), p)
        st, r = key_call(key, "DELETE", "/api/projects/" + p.get("id", "x"))
        check("an owner's key cannot do owner-only things", st == 403, r)

        # what a key can never reach
        for method, path in (("GET", "/api/api_keys"), ("POST", "/api/api_keys"), ("GET", "/api/users"), ("PUT", "/api/me"),
                             ("GET", "/api/accounts"), ("GET", "/api/mcp"), ("PUT", "/api/settings"), ("POST", "/api/worker/token"),
                             ("GET", "/api/auth/status"), ("POST", "/api/skills"), ("GET", "/api/chatsx")):
            st, r = key_call(key, method, path, {} if method != "GET" else None)
            check(f"a key cannot use {method} {path}", st == 403, (st, r))
        check("a wrong key is refused", key_call("pc_" + "0" * 48, "GET", "/api/state")[0] == 401)
        check("a browser call without X-PC2 is still refused", urllib_status("POST", "/api/chats", {"agent_id": a["id"]}) == 403)

        # other people's keys
        st, r = vera.call("POST", "/api/api_keys", {"name": "viewer bot"})
        vkey = r.get("token", "")
        check("a viewer makes a key", st == 201, r)
        check("a viewer's key reads", key_call(vkey, "GET", "/api/chats")[0] == 200)
        check("a viewer's key cannot chat", key_call(vkey, "POST", "/api/chats", {"agent_id": a["id"]})[0] == 403)
        check("keys are per person", len(vera.call("GET", "/api/api_keys")[1]["keys"]) == 1)
        st, r = ece.call("POST", "/api/api_keys", {"name": "ece bot"})
        ekey, eid = r.get("token", ""), r.get("key", {}).get("id", "")
        check("someone else cannot delete my key", o.call("DELETE", "/api/api_keys/" + eid)[0] == 404)
        st, c2 = key_call(ekey, "POST", "/api/chats", {"agent_id": a["id"]})
        check("an editor's key chats", st == 201)
        check("an editor's key does not see the owner's private chat", key_call(ekey, "GET", "/api/chats/" + cid)[0] == 404)

        # ending keys
        check("deleting a key", ece.call("DELETE", "/api/api_keys/" + eid)[0] == 200)
        check("a deleted key stops working", key_call(ekey, "GET", "/api/state")[0] == 401)
        for i in range(9):
            o.call("POST", "/api/api_keys", {"name": f"k{i}"})
        check("at most 10 keys", o.call("POST", "/api/api_keys", {"name": "one too many"})[0] == 409)
        st, r = o.call("PUT", "/api/me", {"password": "new horse pass 2", "old_password": "correct horse 1"})
        check("password change", st == 200, r)
        check("a password change ends the keys", key_call(key, "GET", "/api/state")[0] == 401 and o.call("GET", "/api/api_keys")[1]["keys"] == [])
        o.call("DELETE", "/api/users/vera")
        check("removing a person ends their keys", key_call(vkey, "GET", "/api/state")[0] == 401)
    finally:
        proc.terminate()
        proc.wait(10)
    print(f"\n{len(failures)} failed" if failures else "\nall passed")
    sys.exit(1 if failures else 0)


def urllib_status(method, path, body):
    req = urllib.request.Request(BASE + path, data=json.dumps(body).encode(), method=method, headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status
    except urllib.error.HTTPError as e:
        return e.code


if __name__ == "__main__":
    main()
