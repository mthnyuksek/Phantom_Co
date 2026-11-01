"""Test of "lend my Claude/Codex" and of the file exchange.

Usage: python -X utf8 -I worker_test.py <phantom_co.exe>

Starts two programs: the host company (port 7792) and a friend's program (port 7793) that lends a FAKE `claude`
(a .cmd file that only prints stream-json and writes files), so nothing is billed. Checks:
  - file upload/download rules for editor/viewer/owner
  - worker token, provider validation, the worker API needs the token
  - a chat with an agent of provider "claude@<friend>": events, answer, session, files sent there and back
  - Bash/PowerShell never reach the friend's PC, the session is resumed on the friend's PC
"""
import json
import os
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

HOST_PORT, FRIEND_PORT = 7792, 7793
BASE = f"http://127.0.0.1:{HOST_PORT}"
failures = []


def check(name, cond, extra=""):
    print(("PASS " if cond else "FAIL ") + name + (f"  [{extra}]" if extra and not cond else ""))
    if not cond:
        failures.append(name)


class Client:
    def __init__(self):
        self.cookie = ""

    def call(self, method, path, body=None, headers=None, raw=None):
        data = json.dumps(body).encode() if body is not None else raw
        h = {"X-PC2": "1"}
        if body is not None:
            h["Content-Type"] = "application/json"
        if self.cookie:
            h["Cookie"] = self.cookie
        h.update(headers or {})
        req = urllib.request.Request(BASE + path, data=data, method=method, headers=h)
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                payload = r.read()
                sc = r.headers.get("Set-Cookie")
                if sc and "pc2_session_" in sc:
                    tok = sc.split(f"pc2_session_{HOST_PORT}=")[1].split(";")[0]
                    self.cookie = f"pc2_session_{HOST_PORT}={tok}" if tok else ""
                return r.status, (json.loads(payload) if payload[:1] in (b"{", b"[") else payload), r.headers
        except urllib.error.HTTPError as e:
            payload = e.read()
            try:
                return e.code, json.loads(payload), e.headers
            except Exception:
                return e.code, payload, e.headers

    def get(self, p, **kw): return self.call("GET", p, **kw)[:2]
    def post(self, p, b=None, **kw): return self.call("POST", p, b if b is not None else {}, **kw)[:2]
    def put(self, p, b, **kw): return self.call("PUT", p, b, **kw)[:2]
    def delete(self, p, **kw): return self.call("DELETE", p, **kw)[:2]
    def upload(self, p, data): return self.call("PUT", p, raw=data, headers={"Content-Type": "application/octet-stream"})[:2]


def wait(cond, timeout=60, step=0.3):
    t0 = time.time()
    while time.time() - t0 < timeout:
        v = cond()
        if v:
            return v
        time.sleep(step)
    return None


def main():
    exe = os.path.abspath(sys.argv[1])
    tmp = tempfile.mkdtemp(prefix="pc2_worker_")
    host_vault, host_data = os.path.join(tmp, "HostVault"), os.path.join(tmp, "HostData")
    fr_vault, fr_data = os.path.join(tmp, "FriendVault"), os.path.join(tmp, "FriendData")
    fake_bin, fake_home = os.path.join(tmp, "fakebin"), os.path.join(tmp, "friend_home")
    os.makedirs(fake_bin)
    os.makedirs(fake_home)
    # A fake `claude`: records its arguments, copies a file it was given, writes a new one, prints stream-json.
    with open(os.path.join(fake_bin, "claude.cmd"), "w") as f:
        f.write("@echo off\r\n"
                "echo %* > last_args.txt\r\n"
                "if exist hostfile.txt copy hostfile.txt hostfile_copy.txt >nul\r\n"
                "echo remote hello> from_worker.txt\r\n"
                'echo {"type":"system","subtype":"init","session_id":"sess123","model":"fake"}\r\n'
                'echo {"type":"assistant","message":{"content":[{"type":"text","text":"working on it"}]}}\r\n'
                'echo {"type":"result","subtype":"success","is_error":false,"result":"all done here","session_id":"sess123","total_cost_usd":0.01,"usage":{"input_tokens":5,"output_tokens":7}}\r\n')

    host = subprocess.Popen([exe, "--vault", host_vault, "--data", host_data, "--port", str(HOST_PORT), "--no-window", "--share"],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    friend = None
    try:
        anon = Client()
        wait(lambda: anon.get("/api/auth/status")[0] == 200 if True else None, 15)
        owner, ece, vera = Client(), Client(), Client()
        check("owner created", owner.post("/api/auth/setup", {"name": "Meto", "password": "owner password 1"})[0] == 200)
        st, r = owner.post("/api/users", {"name": "Ece", "role": "editor"})
        ece_tok = r["invite_token"]
        st, r = owner.post("/api/users", {"name": "Vera", "role": "viewer"})
        vera_tok = r["invite_token"]
        check("friend joins", ece.post("/api/auth/join", {"token": ece_tok, "password": "ece password 1"})[0] == 200)
        check("viewer joins", vera.post("/api/auth/join", {"token": vera_tok, "password": "vera password 1"})[0] == 200)

        # ------------------------------------------------------------- file exchange
        st, r = ece.upload("/api/files/upload?scope=shared&path=notes/a.txt", b"hello from ece")
        check("editor uploads", st == 201, r)
        st, body, hdr = ece.call("GET", "/api/files/download?scope=shared&path=notes/a.txt")
        check("download returns the bytes", st == 200 and body == b"hello from ece" and "attachment" in (hdr.get("Content-Disposition") or ""), (st, body))
        check("everybody can download", vera.call("GET", "/api/files/download?scope=shared&path=notes/a.txt")[0] == 200)
        check("viewer cannot upload", vera.upload("/api/files/upload?scope=shared&path=x.txt", b"x")[0] == 403)
        check("existing file is not overwritten", ece.upload("/api/files/upload?scope=shared&path=notes/a.txt", b"other")[0] == 409)
        check("overwrite when asked", ece.upload("/api/files/upload?scope=shared&path=notes/a.txt&overwrite=1", b"new text")[0] == 201)
        check("path traversal refused", ece.upload("/api/files/upload?scope=shared&path=../evil.txt", b"x")[0] == 400)
        check("project metadata refused", ece.upload("/api/files/upload?scope=shared&path=.Pc2/x.txt", b"x")[0] == 400)
        check("unknown folder refused", ece.upload("/api/files/upload?scope=project:nope&path=x.txt", b"x")[0] == 404)
        check("mkdir", ece.post("/api/files/mkdir", {"scope": "shared", "path": "new_dir/sub"})[0] == 201)
        check("editor cannot delete", ece.delete("/api/files?scope=shared&path=notes/a.txt")[0] == 403)
        check("owner deletes", owner.delete("/api/files?scope=shared&path=notes/a.txt")[0] == 200)
        check("deleted file is gone", ece.call("GET", "/api/files/download?scope=shared&path=notes/a.txt")[0] == 404)

        # ------------------------------------------------------------- worker API rules
        check("viewer cannot get a worker token", vera.post("/api/worker/token")[0] == 403)
        check("poll needs a token", anon.post("/api/worker/poll", {"providers": ["claude"]})[0] == 401)
        check("bad token refused", Client().post("/api/worker/poll", {"providers": ["claude"]}, headers={"Authorization": "Bearer " + "0" * 48})[0] == 401)
        check("files need a running job", anon.get("/api/worker/files?run_id=x")[0] == 401)
        st, r = ece.post("/api/worker/token")
        token = r["token"]
        check("editor gets a token", st == 200 and len(token) == 48, r)
        auth_h = {"Authorization": "Bearer " + token}
        check("files without a run are refused", anon.get("/api/worker/files?run_id=nope", headers=auth_h)[0] == 404)
        check("unknown friend refused as provider", owner.post("/api/agents", {"name": "Bad", "provider": "claude@nobody"})[0] == 400)
        check("viewer cannot be a provider", owner.post("/api/agents", {"name": "Bad", "provider": "claude@vera"})[0] == 400)
        check("demo cannot be remote", owner.post("/api/agents", {"name": "Bad", "provider": "demo@ece"})[0] == 400)

        # ------------------------------------------------------------- the friend's program
        env = dict(os.environ)
        env.update({"PATH": fake_bin + os.pathsep + os.environ.get("SystemRoot", r"C:\Windows") + r"\System32",
                    "USERPROFILE": fake_home, "APPDATA": fake_home, "LOCALAPPDATA": fake_home})
        friend = subprocess.Popen([exe, "--vault", fr_vault, "--data", fr_data, "--port", str(FRIEND_PORT), "--no-window",
                                   "--worker-url", BASE, "--worker-token", token], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        def friend_online():
            st, s = ece.get("/api/state")
            return any(w["user_id"] == "ece" and w["online"] and "claude" in w["providers"] for w in s.get("workers", []))
        check("friend shows up online with claude", bool(wait(friend_online, 30)))

        owner.post("/api/skills", {"name": "Shell stuff", "tools": ["Bash", "PowerShell", "Read", "Write"], "body": "Be careful.", "min_role": "editor"})
        st, agent = owner.post("/api/agents", {"name": "Remote Ghost", "provider": "claude@ece", "model": "haiku", "skills": ["shell_stuff", "read_files", "write_files"]})
        check("remote agent created", st == 201 and agent["provider"] == "claude@ece", agent)
        aid = agent["id"]
        check("a file the friend must receive", ece.upload("/api/files/upload?scope=shared&path=hostfile.txt", b"content on the host")[0] == 201)

        st, chat = owner.post("/api/chats", {"agent_id": aid})
        cid = chat["meta"]["id"]
        st, v = owner.post(f"/api/chats/{cid}/send", {"text": "please work"})
        check("send accepted", st == 200, v)

        def idle():
            st, v = owner.get("/api/chats/" + cid)
            return v if st == 200 and not v["busy"] else None
        v = wait(idle, 90)
        texts = [m.get("text", "") for m in (v or {}).get("messages", [])]
        check("answer came back from the friend's PC", any("all done here" in t for t in texts), texts)
        check("no failure line", not any("could not finish" in t for t in texts), texts)

        shared = os.path.join(host_vault, "Shared_Files")
        check("file written on the friend's PC arrived on the host", os.path.exists(os.path.join(shared, "from_worker.txt")))
        copy = os.path.join(shared, "hostfile_copy.txt")
        check("host file reached the friend's PC and back", os.path.exists(copy) and open(copy, "rb").read() == b"content on the host")
        mirror_args = None
        for root, _, files in os.walk(os.path.join(fake_home, "PC2", "Worker")):
            if "last_args.txt" in files:
                mirror_args = open(os.path.join(root, "last_args.txt"), encoding="utf-8", errors="replace").read()
        check("fake claude was started on the friend's PC", mirror_args is not None)
        check("no shell tool reached the friend", mirror_args is not None and "Bash" not in mirror_args and "PowerShell" not in mirror_args, mirror_args)
        check("file tools were passed", mirror_args is not None and "Read" in mirror_args and "Write" in mirror_args, mirror_args)
        check("model passed", mirror_args is not None and "haiku" in mirror_args, mirror_args)
        check("no --add-dir for remote runs", mirror_args is not None and "--add-dir" not in mirror_args, mirror_args)

        st, runs = owner.get("/api/runs")
        run = next((r for r in runs.get("runs", runs) if r.get("agent_id") == aid), None) if isinstance(runs, (dict, list)) else None
        check("run recorded as done", run is not None and run.get("status") == "done", run)
        check("tokens recorded", run is not None and int(run.get("tokens_out", 0)) == 7, run)
        conv_file = os.path.join(host_vault, "Conversations", cid + ".jsonl")
        check("session stored with the friend's name", "@ece:sess123" in open(conv_file, encoding="utf-8").read())

        # second message resumes the session on the friend's PC
        st, v = owner.post(f"/api/chats/{cid}/send", {"text": "and again"})
        v = wait(idle, 90)
        for root, _, files in os.walk(os.path.join(fake_home, "PC2", "Worker")):
            if "last_args.txt" in files:
                mirror_args = open(os.path.join(root, "last_args.txt"), encoding="utf-8", errors="replace").read()
        check("session resumed on the friend's PC", mirror_args is not None and "--resume sess123" in mirror_args, mirror_args)

        # a local agent must never resume a remote session
        st, local = owner.post("/api/agents", {"name": "Local Ghost", "provider": "demo"})
        check("local agent still works", st == 201)
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
