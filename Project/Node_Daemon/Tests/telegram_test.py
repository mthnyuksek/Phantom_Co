"""Telegram bridge test with a fake Telegram (no bot, no internet, demo ghosts only).

Usage: python -X utf8 -I telegram_test.py <absolute path of phantom_co.exe>
The program gets PHANTOM_TELEGRAM_API=http://127.0.0.1:7796, where this script answers getMe, getUpdates, sendMessage...
"""
import json
import os
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT, FAKE = 7794, 7796
BASE = f"http://127.0.0.1:{PORT}"
failures = []
updates, sent, lock = [], [], threading.Lock()
next_id = [1]


def check(name, cond, extra=""):
    print(("PASS " if cond else "FAIL ") + name + (f"  [{extra}]" if extra and not cond else ""))
    if not cond:
        failures.append(name)


class FakeTelegram(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))) or b"{}")
        method = self.path.rsplit("/", 1)[-1]
        result = True
        if method == "getMe":
            result = {"id": 1, "is_bot": True, "username": "phantom_test_bot"}
        elif method == "getUpdates":
            t0 = time.time()
            while True:
                with lock:
                    result = [u for u in updates if u["update_id"] >= body.get("offset", 0)]
                if result or time.time() - t0 > 1:
                    break
                time.sleep(0.1)
        elif method == "sendMessage":
            with lock:
                sent.append({"chat": str(body["chat_id"]), "text": body["text"]})
        data = json.dumps({"ok": True, "result": result}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def say(text, user=42, lang="en"):
    with lock:
        updates.append({"update_id": next_id[0], "message": {"message_id": next_id[0], "date": int(time.time()), "text": text,
                                                              "from": {"id": user, "first_name": "Tester", "language_code": lang},
                                                              "chat": {"id": user, "type": "private", "first_name": "Tester"}}})
        next_id[0] += 1


def reply_after(n, pred, timeout=30):
    """The first message sent after the first n that matches pred."""
    t0 = time.time()
    while time.time() - t0 < timeout:
        with lock:
            for m in sent[n:]:
                if pred(m["text"]):
                    return m["text"]
        time.sleep(0.2)
    with lock:
        return None if not sent[n:] else "NO MATCH; got: " + " || ".join(m["text"][:80] for m in sent[n:])


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
    srv = ThreadingHTTPServer(("127.0.0.1", FAKE), FakeTelegram)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    tmp = tempfile.mkdtemp(prefix="pc2_tg_")
    env = dict(os.environ, PHANTOM_TELEGRAM_API=f"http://127.0.0.1:{FAKE}")
    proc = subprocess.Popen([exe, "--vault", os.path.join(tmp, "Vault"), "--data", os.path.join(tmp, "Data"), "--port", str(PORT), "--no-window"],
                            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        o = Client()
        for _ in range(50):
            try:
                o.call("GET", "/api/auth/status")
                break
            except Exception:
                time.sleep(0.2)
        check("setup", o.call("POST", "/api/auth/setup", {"name": "Meto", "password": "correct horse 1"})[0] == 200)
        check("not listening yet: no code", o.call("POST", "/api/telegram/link", {})[0] == 409)
        st, r = o.call("POST", "/api/mcp", {"catalog": "telegram"})
        check("telegram added", st == 201, r)
        st, r = o.call("PUT", "/api/mcp/telegram", {"secrets": {"bot_token": "123:test"}, "options": {"chat_id": "", "listen": True}})
        check("listening switched on", st == 200 and r["options"]["listen"] is True, r)
        st, a = o.call("POST", "/api/agents", {"name": "Çasper Kaya", "role": "Helper", "provider": "demo"})
        check("demo ghost", st == 201, a)
        t0 = time.time()
        while time.time() - t0 < 20 and o.call("GET", "/api/telegram")[1].get("state") != "on":
            time.sleep(0.3)
        st, s = o.call("GET", "/api/telegram")
        check("bridge is on with the bot's name", s.get("state") == "on" and s.get("bot") == "phantom_test_bot", s)

        n = len(sent); say("/agents")
        check("strangers are told how to connect", (reply_after(n, lambda t: "/link" in t) or "").startswith("This bot belongs"))
        st, info = o.call("POST", "/api/telegram/link", {})
        check("profile gives a code", st == 200 and len(info.get("code", "")) == 6, info)
        n = len(sent); say("/link 000000")
        check("wrong code refused", reply_after(n, lambda t: "wrong" in t) is not None)
        n = len(sent); say("/start link_" + info["code"])
        check("start button links the account", (reply_after(n, lambda t: "Connected" in t) or "").startswith("Connected to Phantom Co. as Meto"))
        check("profile shows the link", o.call("GET", "/api/state")[1]["telegram"]["linked"] == [{"name": "Tester"}])

        n = len(sent); say("/agents")
        lst = reply_after(n, lambda t: "Ghosts" in t) or ""
        check("/agents lists the ghost with a Telegram command", "/casper_kaya — Çasper Kaya · Helper" in lst, lst)
        n = len(sent); say("hello?")
        check("plain text without a ghost asks who", reply_after(n, lambda t: "Who should get this" in t) is not None)
        n = len(sent); say("/nobody hi")
        check("unknown ghost", reply_after(n, lambda t: "no ghost called /nobody" in t) is not None)
        n = len(sent); say("/casper_kaya hello there")
        ans = reply_after(n, lambda t: t.startswith("👻 Çasper Kaya:"), 40)
        check("the ghost's answer comes back", ans and ans.startswith("👻 Çasper Kaya:"), ans)
        chats = o.call("GET", "/api/chats")[1]["chats"]
        tchat = next((c for c in chats if c.get("title") == "Telegram · Çasper Kaya"), None)
        check("a normal chat holds it", tchat is not None, [c.get("title") for c in chats])
        n = len(sent); say("and again")
        check("plain text goes to the current ghost", (reply_after(n, lambda t: t.startswith("👻"), 40) or "").startswith("👻 Çasper Kaya:"))
        n = len(sent); say("/new")
        check("/new makes it forget", reply_after(n, lambda t: "forgot the chat" in t) is not None)
        v = o.call("GET", "/api/chats/" + tchat["id"])[1] if tchat else {"messages": []}
        check("the chat got a new session", any(m.get("t") == "reset" for m in v["messages"]), [m.get("t") for m in v["messages"]])
        n = len(sent); say("/yes")
        check("/yes with nothing waiting", reply_after(n, lambda t: "Nothing is waiting" in t) is not None)
        n = len(sent); say("/agents", lang="tr")
        check("Turkish for a Turkish Telegram", reply_after(n, lambda t: "Şirketin hayaletleri" in t) is not None)

        # the person goes on in the app: answers stay in the app
        if tchat:
            n = len(sent)
            o.call("POST", f"/api/chats/{tchat['id']}/send", {"text": "from the app"})
            time.sleep(4)
            with lock:
                check("answers to app messages stay in the app", not any(m["text"].startswith("👻") for m in sent[n:]), sent[n:])
        if "--real" in sys.argv:  # a Claude ghost sends a Telegram message: the approval is asked and answered in Telegram
            st, g = o.call("POST", "/api/agents", {"name": "Quill", "role": "Messenger", "provider": "claude", "model": "haiku", "skills": ["telegram"],
                                                   "persona": "You do exactly what you are asked, with the tools you have."})
            check("claude ghost", st == 201, g)
            o.call("PUT", "/api/mcp/telegram", {"approve": True, "approve_tools": ["send_message"]})
            n = len(sent); say("/quill Call mcp__telegram__read_messages, then send the text 'pumpkin pie' with mcp__telegram__send_message to chat_id 777. Then say done.")
            ask = reply_after(n, lambda t: t.startswith("🔐"), 180)
            check("the approval request comes to Telegram", ask and "send_message" in ask and "pumpkin pie" in ask, ask)
            n = len(sent); say("/yes")
            check("approved from Telegram", reply_after(n, lambda t: t.startswith("✓ Meto approved"), 30) is not None)
            delivered = reply_after(n, lambda t: t == "pumpkin pie", 120)
            check("the tool sent the message after /yes", delivered == "pumpkin pie", delivered)
            fin = reply_after(n, lambda t: t.startswith("👻 Quill:"), 180)
            check("the ghost's answer comes back", fin is not None, fin)
            with lock:
                check("sent to the chat the ghost named", any(m["chat"] == "777" and m["text"] == "pumpkin pie" for m in sent))

        n = len(sent); say("/unlink")
        check("/unlink", reply_after(n, lambda t: "Disconnected" in t) is not None)
        check("profile no longer linked", o.call("GET", "/api/state")[1]["telegram"]["linked"] == [])
    finally:
        proc.terminate()
        try:
            proc.wait(5)
        except Exception:
            proc.kill()
        srv.shutdown()
    print("\nALL PASSED" if not failures else f"\n{len(failures)} FAILED: {failures}")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
