"""Reminders MCP server against a fake Worker, through the --builtin-call test hook.

Usage: python -X utf8 -I reminders_test.py <absolute path of phantom_co.exe>
Nothing leaves this PC: a local fake Worker answers. The real Worker code is checked by reminders_worker_test.mjs (node).
"""
import json
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

failures = []
seen = []  # (method, path, auth, body)
KEY = "k" * 20


def check(name, cond, extra=""):
    if not cond:
        failures.append(name)
        print("FAIL", name, extra)


class Fake(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def send(self, status, obj):
        data = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def handle_any(self, body):
        seen.append((self.command, self.path, self.headers.get("Authorization", ""), body))
        if self.headers.get("Authorization") != "Bearer " + KEY:
            return self.send(401, {"error": "wrong key"})
        bare = self.path.split("?")[0]
        if bare == "/health":
            return self.send(200, {"ok": True, "waiting": 3, "telegram": True})
        if bare == "/reminders" and self.command == "POST":
            return self.send(200, {"ids": list(range(1, len(body["reminders"]) + 1))})
        if bare == "/reminders" and self.command == "GET":
            return self.send(200, {"reminders": [{"id": 7, "at": int(time.time()) + 3600, "text": "Chess club form"}]})
        if bare == "/reminders/cancel":
            return self.send(200, {"removed": len(body["ids"])})
        self.send(404, {"error": "not found"})

    def do_GET(self):
        self.handle_any(None)

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        self.handle_any(json.loads(self.rfile.read(n) or b"{}"))


def run(exe, req):
    p = subprocess.run([exe, "--builtin-call"], input=json.dumps(req).encode(), capture_output=True, timeout=60)
    return json.loads(p.stdout.decode().strip().splitlines()[-1])


def text(r):
    return r["content"][0]["text"]


def main():
    exe = sys.argv[1]
    srv = ThreadingHTTPServer(("127.0.0.1", 0), Fake)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    url = f"http://127.0.0.1:{srv.server_address[1]}/"  # a trailing slash is tolerated
    sec = {"worker_url": url, "worker_key": KEY}
    tg = ["555"]

    def call(tool, args, secrets=sec, ids=tg):
        return run(exe, {"builtin": "reminders", "tool": tool, "args": args, "secrets": secrets, "telegram_ids": ids})

    # Settings > MCP check
    r = run(exe, {"builtin": "reminders", "tool": "", "secrets": sec})
    check("check ok", r["ok"] and "3 reminder(s) waiting" in r["text"], r)
    r = run(exe, {"builtin": "reminders", "tool": "", "secrets": {"worker_url": url, "worker_key": "wrong"}})
    check("check wrong key", not r["ok"] and "refused the key" in r["text"], r)
    r = run(exe, {"builtin": "reminders", "tool": "", "secrets": {}})
    check("check empty", not r["ok"] and "No Worker" in r["text"], r)
    r = run(exe, {"builtin": "reminders", "tool": "", "secrets": {"worker_url": "ftp://x", "worker_key": KEY}})
    check("check scheme", not r["ok"], r)

    # schedule: local time -> unix seconds, one POST for the whole list, to the asker's chat
    soon = time.localtime(time.time() + 7200)
    at = time.strftime("%Y-%m-%d %H:%M", soon)
    want = int(time.mktime(time.strptime(at, "%Y-%m-%d %H:%M")))
    seen.clear()
    r = call("schedule_reminders", {"reminders": [{"at": at, "text": "Chess club: send the form"}, {"at": at.replace(" ", "T"), "text": "Robotics: order parts"}]})
    check("schedule ok", not r["isError"] and "Scheduled 2" in text(r), r)
    post = [s for s in seen if s[0] == "POST"]
    check("one post", len(post) == 1, seen)
    if post:
        body = post[0][3]
        check("chat id", body["chat_id"] == "555", body)
        check("unix time", [x["at"] for x in body["reminders"]] == [want, want], body)
        check("auth header", post[0][2] == "Bearer " + KEY)

    # all or nothing: one bad entry sends nothing
    seen.clear()
    r = call("schedule_reminders", {"reminders": [{"at": at, "text": "ok"}, {"at": "2020-01-01 10:00", "text": "past"}, {"at": "soon", "text": "x"}, {"at": at, "text": ""}]})
    check("bad list refused", r["isError"] and "Nothing was scheduled" in text(r) and "#2" in text(r) and "#3" in text(r) and "#4" in text(r), r)
    check("bad list sent nothing", not seen, seen)
    r = call("schedule_reminders", {"reminders": [{"at": "2027-02-31 10:00", "text": "no such day"}]})
    check("impossible date", r["isError"], r)
    r = call("schedule_reminders", {"reminders": [{"at": at, "text": "x"}] * 101})
    check("batch limit", r["isError"] and "100" in text(r), r)
    r = call("schedule_reminders", {})
    check("empty list", r["isError"], r)

    # long text is cut at 500 characters
    seen.clear()
    call("schedule_reminders", {"reminders": [{"at": at, "text": "ş" * 900}]})
    sent = [s for s in seen if s[0] == "POST"][0][3]["reminders"][0]["text"]
    check("text cut", 200 < len(sent) < 400 and len(sent.encode()) <= 510, len(sent.encode()))  # cut at 500 bytes (+ "…"), well under the Worker's 500 characters

    # no Telegram linked: nothing leaves, the ghost is told what to do
    seen.clear()
    r = call("schedule_reminders", {"reminders": [{"at": at, "text": "x"}]}, ids=[])
    check("no telegram", r["isError"] and "Telegram" in text(r) and not seen, r)

    # no Worker set up
    r = call("list_reminders", {}, secrets={})
    check("not set up", r["isError"] and "not set up" in text(r), r)

    # list and cancel
    r = call("list_reminders", {})
    check("list", "#7" in text(r) and "Chess club form" in text(r), r)
    check("list asks own chat", any("chat=555" in s[1] for s in seen if s[0] == "GET"), seen)
    r = call("cancel_reminders", {"ids": [7, 8, "x"]})
    check("cancel", not r["isError"] and "Cancelled 2" in text(r), r)
    r = call("cancel_reminders", {"ids": []})
    check("cancel empty", r["isError"], r)

    # wrong key
    r = call("list_reminders", {}, secrets={"worker_url": url, "worker_key": "wrong"})
    check("wrong key tool", r["isError"] and "refused the key" in text(r), r)
    # unreachable
    r = call("list_reminders", {}, secrets={"worker_url": "http://127.0.0.1:1/", "worker_key": KEY})
    check("unreachable", r["isError"] and "could not be reached" in text(r), r)

    srv.shutdown()
    print("FAILED: " + ", ".join(failures) if failures else "all reminders checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
