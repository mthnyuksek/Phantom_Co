"""Test of "Internet access" (Cloudflare quick tunnel). NEEDS THE INTERNET: downloads cloudflared (about 40 MB)
into a temporary LOCALAPPDATA and opens a real tunnel.

Usage: python -X utf8 -I tunnel_test.py <phantom_co.exe>

Checks: install, start, the https address works from outside with the tunnel Host/Origin, a new account cannot be
created through the tunnel (it is "this PC only"), a friend's program lends a FAKE claude over https, and the tunnel
stops again.
"""
import json
import os
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

PORT, FRIEND_PORT = 7794, 7795
LOCAL = f"http://127.0.0.1:{PORT}"
failures = []


def check(name, cond, extra=""):
    print(("PASS " if cond else "FAIL ") + name + (f"  [{extra}]" if extra and not cond else ""))
    if not cond:
        failures.append(name)


class Client:
    def __init__(self, base):
        self.base, self.cookie = base, ""

    def call(self, method, path, body=None, headers=None):
        data = json.dumps(body).encode() if body is not None else None
        h = {"X-PC2": "1", "User-Agent": "pc2-test"}
        if body is not None:
            h["Content-Type"] = "application/json"
        if self.cookie:
            h["Cookie"] = self.cookie
        h.update(headers or {})
        req = urllib.request.Request(self.base + path, data=data, method=method, headers=h)
        try:
            with urllib.request.urlopen(req, timeout=40) as r:
                raw = r.read()
                sc = r.headers.get("Set-Cookie")
                if sc and "pc2_session_" in sc:
                    self.cookie = sc.split(";")[0]
                return r.status, (json.loads(raw) if raw[:1] in (b"{", b"[") else raw)
        except urllib.error.HTTPError as e:
            raw = e.read()
            try:
                return e.code, json.loads(raw)
            except Exception:
                return e.code, raw

    def get(self, p, **kw): return self.call("GET", p, **kw)
    def post(self, p, b=None, **kw): return self.call("POST", p, b if b is not None else {}, **kw)


def wait(cond, timeout=60, step=0.5):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            v = cond()
        except Exception:
            v = None
        if v:
            return v
        time.sleep(step)
    return None


def main():
    exe = os.path.abspath(sys.argv[1])
    tmp = tempfile.mkdtemp(prefix="pc2_tunnel_")
    host_env = dict(os.environ, LOCALAPPDATA=os.path.join(tmp, "host_local"))
    os.makedirs(host_env["LOCALAPPDATA"])
    fake_bin, fake_home = os.path.join(tmp, "fakebin"), os.path.join(tmp, "friend_home")
    os.makedirs(fake_bin)
    os.makedirs(fake_home)
    with open(os.path.join(fake_bin, "claude.cmd"), "w") as f:
        f.write("@echo off\r\n"
                'echo {"type":"system","subtype":"init","session_id":"s1","model":"fake"}\r\n'
                'echo {"type":"result","subtype":"success","is_error":false,"result":"answer over the tunnel","session_id":"s1","usage":{"input_tokens":1,"output_tokens":2}}\r\n')
    host = subprocess.Popen([exe, "--vault", os.path.join(tmp, "V"), "--data", os.path.join(tmp, "D"), "--port", str(PORT), "--no-window"],
                            env=host_env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    friend = None
    try:
        owner = Client(LOCAL)
        check("host starts", bool(wait(lambda: owner.get("/api/auth/status")[0] == 200, 15)))
        check("owner created", owner.post("/api/auth/setup", {"name": "Meto", "password": "owner password 1"})[0] == 200)
        st, r = owner.post("/api/users", {"name": "Ece", "role": "editor"})
        ece = Client(LOCAL)
        check("friend joins", ece.post("/api/auth/join", {"token": r["invite_token"], "password": "ece password 1"})[0] == 200)
        check("friend cannot use the tunnel API", ece.get("/api/tunnel")[0] == 403)

        st, t = owner.get("/api/tunnel")
        check("tunnel off at start", st == 200 and t["state"] == "off" and not t["url"], t)
        check("start without cloudflared fails cleanly", owner.post("/api/tunnel", {"action": "start"})[1]["state"] in ("error", "starting"))
        owner.post("/api/tunnel", {"action": "install"})
        t = wait(lambda: (lambda s: s if s["state"] != "installing" else None)(owner.get("/api/tunnel")[1]), 240, 1)
        check("cloudflared installed", bool(t) and t["installed"], t)
        owner.post("/api/tunnel", {"action": "start"})
        t = wait(lambda: (lambda s: s if s["state"] in ("on", "error") else None)(owner.get("/api/tunnel")[1]), 90, 1)
        check("tunnel is on with an https address", bool(t) and t["state"] == "on" and t["url"].startswith("https://") and t["url"].endswith(".trycloudflare.com"), t)
        if not t or t["state"] != "on":
            return
        url = t["url"]
        print("   tunnel:", url)

        outside = Client(url)
        st = wait(lambda: outside.get("/api/auth/status")[0] == 200, 90, 2)
        check("page and API reachable over https", bool(st))
        check("html served through the tunnel", b"Phantom Co." in outside.get("/")[1])
        st, r = outside.post("/api/auth/login", {"name": "ece", "password": "wrong wrong 1"})
        check("login over the tunnel works (wrong password = 401, not 403)", st == 401, (st, r))
        st, r = outside.post("/api/auth/login", {"name": "ece", "password": "ece password 1"})
        check("sign in from outside", st == 200, (st, r))
        check("signed-in API over the tunnel", outside.get("/api/state")[0] == 200)
        check("foreign Origin refused over the tunnel", Client(url).post("/api/auth/login", {"name": "a", "password": "b"}, headers={"Origin": "https://evil.example.com"})[0] == 403)
        check("cannot open an owner account through the tunnel", Client(url).post("/api/auth/register", {"name": "Intruder", "password": "intruder pass 1"})[0] == 403)
        check("local register still works", Client(LOCAL).post("/api/auth/register", {"name": "Second", "password": "second pass 12"})[0] == 200)
        check("tunnel api is owner only from outside", outside.get("/api/tunnel")[0] == 403)

        # the friend's program lends a fake claude, over https
        st, r = ece.post("/api/worker/token")
        token = r["token"]
        env = dict(os.environ, PATH=fake_bin + os.pathsep + os.environ.get("SystemRoot", r"C:\Windows") + r"\System32",
                   USERPROFILE=fake_home, APPDATA=fake_home, LOCALAPPDATA=fake_home)
        friend = subprocess.Popen([exe, "--vault", os.path.join(tmp, "FV"), "--data", os.path.join(tmp, "FD"), "--port", str(FRIEND_PORT), "--no-window",
                                   "--worker-url", url, "--worker-token", token], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        online = wait(lambda: any(w["user_id"] == "ece" and w["online"] for w in ece.get("/api/state")[1].get("workers", [])), 60, 1)
        check("friend lends its AI over https", bool(online))
        st, agent = owner.post("/api/agents", {"name": "Far Ghost", "provider": "claude@ece"})
        st, chat = owner.post("/api/chats", {"agent_id": agent["id"]})
        cid = chat["meta"]["id"]
        owner.post(f"/api/chats/{cid}/send", {"text": "hi"})
        v = wait(lambda: (lambda x: x if x and not x["busy"] else None)(owner.get("/api/chats/" + cid)[1]), 90, 1)
        texts = [m.get("text", "") for m in (v or {}).get("messages", [])]
        check("answer arrived through the tunnel", any("answer over the tunnel" in x for x in texts), texts)

        owner.post("/api/tunnel", {"action": "stop"})
        check("tunnel stops", owner.get("/api/tunnel")[1]["state"] == "off")
        check("outside is refused after stop", wait(lambda: Client(url).get("/api/auth/status")[0] != 200, 30, 1) is not None)
    finally:
        for p in (friend, host):
            if p:
                p.terminate()
                try:
                    p.wait(5)
                except Exception:
                    p.kill()
    print()
    print("ALL PASSED" if not failures else f"{len(failures)} FAILED: {failures}")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
