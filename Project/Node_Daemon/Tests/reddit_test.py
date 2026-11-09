"""Reddit MCP server against a fake Reddit (PHANTOM_REDDIT_API), through the --builtin-call test hook.

Usage: python -X utf8 -I reddit_test.py <absolute path of phantom_co.exe>
Nothing is sent to the real Reddit: public feeds, the sign-in, reading through the API, posting and its refusals.
"""
import base64
import json
import os
import subprocess
import sys
import threading
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

failures = []
seen = []  # (method, path, headers, body)

ATOM = """<?xml version="1.0" encoding="UTF-8"?><feed xmlns="http://www.w3.org/2005/Atom"><category term="feedlevel" label="r/feedlevel"/><title>x</title>
<entry><author><name>/u/alice</name></author><category term="golang" label="r/golang"/>
<content type="html">&lt;p&gt;Hello &amp;amp; welcome&lt;/p&gt; &amp;#32; submitted by &amp;#32; &lt;a href=&quot;u&quot;&gt; /u/alice &lt;/a&gt;</content>
<id>t3_abc12</id><link href="https://www.reddit.com/r/golang/comments/abc12/first/" /><updated>2026-11-09T13:27:16+00:00</updated><title>First &amp;amp; best</title></entry>
<entry><author><name>/u/bob</name></author><category term="golang" label="r/golang"/><content type="html">Nice post</content>
<id>t1_zzz9</id><link href="https://www.reddit.com/c/" /><updated>2026-11-08T10:00:00+00:00</updated><title>bob on First</title></entry></feed>"""

POST = {"title": "API title", "subreddit": "golang", "author": "carol", "score": 7, "num_comments": 1, "name": "t3_abc12", "permalink": "/r/golang/comments/abc12/x/",
        "created_utc": 1762694400.0, "is_self": True, "selftext": "body text"}


class Fake(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def send(self, status, text, ctype="application/json"):
        data = text.encode()
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def handle_any(self, body):
        path = self.path
        seen.append((self.command, path, dict(self.headers), body))
        bare = path.split("?")[0]
        if bare == "/api/v1/access_token":
            form = urllib.parse.parse_qs(body)
            if self.headers.get("Authorization") != "Basic " + base64.b64encode(b"goodid:goodsecret").decode():
                return self.send(401, '{"message": "Unauthorized"}')
            if form.get("grant_type") == ["password"] and form.get("password") != ["rightpass"]:
                return self.send(200, '{"error": "invalid_grant"}')
            return self.send(200, json.dumps({"access_token": "tok-" + form["grant_type"][0], "expires_in": 3600}))
        if bare.endswith(".rss"):
            if "/r/limited/" in bare:
                return self.send(429, "slow down", "text/plain")
            return self.send(200, ATOM, "application/atom+xml")
        if not self.headers.get("Authorization", "").startswith("Bearer "):
            return self.send(403, "{}")
        if bare == "/api/v1/me":
            return self.send(200, '{"name": "phantombot"}')
        if bare == "/comments/abc12":
            reply = {"kind": "t1", "data": {"author": "dan", "score": 1, "name": "t1_b", "body": "inner reply", "replies": ""}}
            comment = {"kind": "t1", "data": {"author": "eve", "score": 2, "name": "t1_a", "body": "top comment", "replies": {"data": {"children": [reply]}}}}
            return self.send(200, json.dumps([{"data": {"children": [{"kind": "t3", "data": POST}]}}, {"data": {"children": [comment, {"kind": "more", "data": {}}]}}]))
        if bare == "/api/submit":
            if "title=bad" in body:
                return self.send(200, json.dumps({"json": {"errors": [["RATELIMIT", "you are doing that too much", "ratelimit"]]}}))
            return self.send(200, json.dumps({"json": {"errors": [], "data": {"url": "https://www.reddit.com/r/golang/comments/new1/"}}}))
        if bare == "/api/comment":
            return self.send(200, json.dumps({"json": {"errors": [], "data": {"things": []}}}))
        if bare.endswith("/hot") or bare.endswith("/search"):
            return self.send(200, json.dumps({"data": {"children": [{"kind": "t3", "data": POST}]}}))
        return self.send(404, "{}")

    def do_GET(self):
        self.handle_any("")

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        self.handle_any(self.rfile.read(n).decode())


def check(name, cond, extra=""):
    print(("PASS " if cond else "FAIL ") + name + (f"  [{extra}]" if extra and not cond else ""))
    if not cond:
        failures.append(name)


def main():
    exe = sys.argv[1]
    server = ThreadingHTTPServer(("127.0.0.1", 0), Fake)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    env = dict(os.environ, PHANTOM_REDDIT_API=f"http://127.0.0.1:{server.server_address[1]}")

    def call(tool, args=None, secrets=None):
        p = subprocess.run([exe, "--builtin-call"], input=json.dumps({"builtin": "reddit", "tool": tool, "args": args or {}, "secrets": secrets or {}}).encode(),
                           capture_output=True, env=env, timeout=30)
        r = json.loads(p.stdout.decode("utf-8"))
        return r if not tool else (r["content"][0]["text"], r["isError"])

    app = {"client_id": "goodid", "client_secret": "goodsecret"}
    full = dict(app, username="phantombot", password="rightpass")

    # no app: public feeds
    t, err = call("list_posts", {"subreddit": "r/golang", "limit": 2})
    check("feed: posts listed", not err and "First & best" in t and "u/alice" in t and "r/golang" in t and "t3_abc12" in t, t)
    check("feed: text decoded, footer cut", "Hello & welcome" in t and "submitted by" not in t, t)
    check("feed: asks the rss address", seen[-1][1] == "/r/golang/hot.rss?limit=2", seen[-1][1])
    check("feed: names the program, no browser disguise", seen[-1][2].get("User-Agent", "").startswith("windows:phantom_co:v"), seen[-1][2])
    t, err = call("list_posts", {"subreddit": "limited"})
    check("feed: rate limit explained", err and "limits reading" in t, t)
    t, err = call("search", {"query": "a b", "subreddit": "golang"})
    check("feed: search in one community", not err and seen[-1][1].startswith("/r/golang/search.rss?q=a%20b") and "restrict_sr=on" in seen[-1][1], seen[-1][1])
    t, err = call("read_post", {"post": "https://www.reddit.com/r/golang/comments/abc12/first/"})
    check("feed: post with comments", not err and seen[-1][1].startswith("/comments/abc12.rss") and "Comments:" in t and "u/bob" in t and "Nice post" in t, t)
    t, err = call("submit_post", {"subreddit": "golang", "title": "x", "text": "y"})
    check("no account: posting refused, nothing sent", err and "Settings > MCP > Reddit" in t and not any(s[1] == "/api/submit" for s in seen), t)
    r = call("")
    check("check without an app", r["ok"] and "public feeds" in r["text"], r)

    # bad input
    t, err = call("list_posts", {"subreddit": "../../etc"})
    check("bad subreddit refused", err, t)
    t, err = call("read_post", {"post": "t3_a/../b"})
    check("bad post id refused", err, t)

    # an app
    t, err = call("list_posts", {"subreddit": "golang"}, app)
    check("app: read through the API", not err and "API title" in t and "7 points" in t, t)
    check("app: app-only token", seen[-1][2].get("Authorization") == "Bearer tok-client_credentials", seen[-1][2])
    t, err = call("list_posts", {"subreddit": "golang"}, {"client_id": "goodid", "client_secret": "wrong"})
    check("wrong secret explained", err and "refused this app" in t, t)
    r = call("", secrets=app)
    check("check: app without an account", r["ok"] and "without an account" in r["text"], r)
    t, err = call("submit_post", {"subreddit": "golang", "title": "x", "text": "y"}, app)
    check("app without an account cannot post", err and "Settings > MCP > Reddit" in t, t)

    # an app and an account
    r = call("", secrets=full)
    check("check names the account", r["ok"] and r["text"] == "u/phantombot", r)
    r = call("", secrets=dict(full, password="wrong"))
    check("wrong password explained", not r["ok"] and "user name or password" in r["text"], r)
    t, err = call("read_post", {"post": "t3_abc12"}, full)
    check("account: comments with replies", not err and "top comment" in t and "  - u/dan" in t and "inner reply" in t, t)
    t, err = call("submit_post", {"subreddit": "golang", "title": "Hello", "text": "Body"}, full)
    check("post published", not err and "comments/new1" in t, t)
    sent = urllib.parse.parse_qs([s for s in seen if s[1] == "/api/submit"][-1][3])
    check("post has the right fields", sent["kind"] == ["self"] and sent["sr"] == ["golang"] and sent["text"] == ["Body"] and sent["api_type"] == ["json"], sent)
    check("user agent names the account", "/u/phantombot" in [s for s in seen if s[1] == "/api/submit"][-1][2].get("User-Agent", ""))
    t, err = call("submit_post", {"subreddit": "golang", "title": "bad", "url": "https://example.com"}, full)
    check("Reddit's refusal reported", err and "doing that too much" in t, t)
    t, err = call("submit_post", {"subreddit": "golang", "title": "x", "text": "a", "url": "https://example.com"}, full)
    check("text and url together refused", err, t)
    t, err = call("comment", {"parent": "t1_abc", "text": "Thanks"}, full)
    sent = urllib.parse.parse_qs([s for s in seen if s[1] == "/api/comment"][-1][3])
    check("comment published", not err and sent["thing_id"] == ["t1_abc"] and sent["text"] == ["Thanks"], (t, sent))

    server.shutdown()
    print(f"\n{len(failures)} failures" if failures else "\nALL PASSED")
    sys.exit(1 if failures else 0)


main()
