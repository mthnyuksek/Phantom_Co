"""Prompt shortcuts test: API, roles and checks (no Telegram, no internet, demo ghosts only).

Usage: python -X utf8 -I shortcuts_test.py <absolute path of phantom_co.exe>
The Telegram side ("/restoran Kadıköy" sent to a ghost) is checked in telegram_test.py.
"""
import json
import os
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

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


def main():
    exe = sys.argv[1]
    tmp = tempfile.mkdtemp(prefix="pc2_sc_")
    vault = os.path.join(tmp, "Vault")
    proc = subprocess.Popen([exe, "--vault", vault, "--data", os.path.join(tmp, "Data"), "--port", str(PORT), "--no-window"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        o, ece, ben, vera = Client(), Client(), Client(), Client()
        for _ in range(50):
            try:
                o.call("GET", "/api/auth/status")
                break
            except Exception:
                time.sleep(0.2)
        check("setup", o.call("POST", "/api/auth/setup", {"name": "Meto", "password": "correct horse 1"})[0] == 200)
        check("vault has a Shortcuts folder", os.path.isdir(os.path.join(vault, "Shortcuts")))
        for who, name, role in ((ece, "Ece", "editor"), (ben, "Ben", "editor"), (vera, "Vera", "viewer")):
            st, r = o.call("POST", "/api/users", {"name": name, "role": role})
            who.call("POST", "/api/auth/join", {"token": r["invite_token"], "password": name.lower() + " password 1"})
        st, a = o.call("POST", "/api/agents", {"name": "Çasper", "provider": "demo"})
        check("demo ghost", st == 201, a)

        # making
        st, s = ece.call("POST", "/api/shortcuts", {"command": "/Çorba Bul!", "title": "Find soup", "body": "Find soup places near {args}.", "agent_id": a["id"]})
        check("an editor makes a shortcut", st == 201 and s["command"] == "corba_bul" and s["created_by"] == "ece" and s["agent_id"] == a["id"], s)
        sid = s.get("id", "")
        f = os.path.join(vault, "Shortcuts", sid + ".md")
        check("it is a Markdown file", os.path.exists(f) and "Find soup places near {args}." in open(f, encoding="utf-8").read())
        check("a viewer cannot make one", vera.call("POST", "/api/shortcuts", {"command": "x1", "body": "hi"})[0] == 403)
        check("no command", ece.call("POST", "/api/shortcuts", {"command": "  !! ", "body": "hi"})[0] == 400)
        check("no text", ece.call("POST", "/api/shortcuts", {"command": "empty", "body": " "})[0] == 400)
        check("a program command is refused", ece.call("POST", "/api/shortcuts", {"command": "agents", "body": "hi"})[0] == 400)
        check("the Turkish help command is refused", ece.call("POST", "/api/shortcuts", {"command": "YARDIM", "body": "hi"})[0] == 400)
        st, r = ece.call("POST", "/api/shortcuts", {"command": "casper", "body": "hi"})
        check("a ghost's command is refused", st == 400 and "Çasper" in r.get("error", ""), r)
        check("a ghost's id is refused too", ece.call("POST", "/api/shortcuts", {"command": a["id"], "body": "hi"})[0] == 400)
        check("a duplicate is refused", ben.call("POST", "/api/shortcuts", {"command": "Corba-Bul", "body": "hi"})[0] == 400)
        check("an unknown ghost is refused", ece.call("POST", "/api/shortcuts", {"command": "other", "body": "hi", "agent_id": "nobody"})[0] == 400)

        # seeing
        st, ben_s = ben.call("POST", "/api/shortcuts", {"command": "summary", "body": "Summarise this:"})
        check("another editor makes one too", st == 201 and ben_s["created_by"] == "ben", ben_s)
        names = [x["command"] for x in ben.call("GET", "/api/state")[1]["shortcuts"]]
        check("everybody who can chat sees all of them, by command", names == ["corba_bul", "summary"], names)
        check("a viewer's state has none", vera.call("GET", "/api/state")[1]["shortcuts"] == [])
        check("a viewer cannot list them", vera.call("GET", "/api/shortcuts")[0] == 403)
        check("an editor lists them", len(ece.call("GET", "/api/shortcuts")[1]["shortcuts"]) == 2)

        # changing
        st, r = ben.call("PUT", "/api/shortcuts/" + sid, {"body": "Hacked"})
        check("another editor cannot change it", st == 403, r)
        check("another editor cannot delete it", ben.call("DELETE", "/api/shortcuts/" + sid)[0] == 403)
        st, r = ece.call("PUT", "/api/shortcuts/" + sid, {"command": "corba_bul", "title": "Soup", "body": "Soup near {args}.", "agent_id": ""})
        check("the maker changes it (same command is fine)", st == 200 and r["body"] == "Soup near {args}." and r["agent_id"] == "" and r["id"] == sid, r)
        st, r = ece.call("PUT", "/api/shortcuts/" + sid, {"command": "summary"})
        check("renaming onto another shortcut is refused", st == 400, r)
        st, r = o.call("PUT", "/api/shortcuts/" + sid, {"command": "soup"})
        check("the owner changes any shortcut", st == 200 and r["command"] == "soup" and r["body"] == "Soup near {args}.", r)
        check("lookup by id works after a rename", ece.call("GET", "/api/state")[1]["shortcuts"][1]["command"] == "summary")
        check("unknown shortcut", o.call("PUT", "/api/shortcuts/nothing", {"body": "x"})[0] == 404)

        # deleting
        check("the maker deletes", ece.call("DELETE", "/api/shortcuts/" + sid)[0] == 200 and not os.path.exists(f))
        check("it is gone", ece.call("DELETE", "/api/shortcuts/" + sid)[0] == 404)
        check("the owner deletes another's", o.call("DELETE", "/api/shortcuts/" + ben_s["id"])[0] == 200)
        check("the list is empty again", o.call("GET", "/api/shortcuts")[1]["shortcuts"] == [])
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
