---
name: Visual scripts
description: Write visual scripts (Scripts tab) for the person: saved as drafts that only a person can run. No tools, about 1000 tokens.
tools:
dirs:
min_role: editor
default: true
---
You can write a visual script: a plan of ghost prompts joined like code blocks, which Phantom Co. runs block by block. Write one when the person asks for a repeatable flow ("first X researches, then if there is enough Claude left Y builds it, 30 minutes later Z presents it"). Put it in your answer as one JSON object inside `<script name="Short name">...</script>`. It is saved as a **draft** in the Scripts tab; it never runs until the person checks it and presses Run. Tell the person that in one sentence. Do not run anything yourself.

Format:
```
{"description": "...", "project_id": "",
 "vars": [{"name": "score", "type": "Number", "init": "0"}],
 "nodes": [{"id": "s", "type": "start", "x": 0, "y": 0},
           {"id": "p1", "type": "prompt", "x": 0, "y": 120, "label": "Research",
            "props": {"agent_id": "<ghost id>", "project_id": "", "text": "...", "target": "score"}},
           {"id": "c1", "type": "if", "x": 0, "y": 260, "props": {"expr": "claude_left_5h > 50 and score >= 7"}},
           {"id": "w1", "type": "delay", "x": -120, "y": 400, "props": {"mode": "for", "expr": "30"}},
           {"id": "e", "type": "end", "x": 0, "y": 540}],
 "edges": [{"from": "s", "to": "p1"}, {"from": "p1", "to": "c1"},
           {"from": "c1", "port": "true", "to": "w1"}, {"from": "c1", "port": "false", "to": "e"}, {"from": "w1", "to": "e"}]}
```
Blocks (`type`):
- `start`: exactly one; the run begins here.
- `prompt`: `agent_id` = a ghost id from your team list (not a name); `text` = the job, may use `{var}` to put a value in; `project_id` "" = the script's project; `target` (optional) = a variable that gets a value from the ghost's answer (the ghost is asked to end with `<value>...</value>`).
- `set`: `{"var": "score", "expr": "score + 1"}`.
- `if`: `{"expr": "..."}` must give true/false; arrows leave it with `"port": "true"` or `"port": "false"`.
- `delay`: `{"mode": "for", "expr": "30"}` (minutes) or `{"mode": "until", "expr": "16:00"}`.
- `end`: optional; nothing comes after it.

Arrows go from a block's output to the next block (`port` is `"out"` except for if). One block may lead to several: they run at the same time. A block with several arrows coming in waits for all of them. **No loops**: arrows must never lead back up. Every block must be reachable from start. Place blocks top to bottom (y grows by ~140 per step, side branches at x ±220).

Types and values (values in the file are texts): `Number` with up to 2 decimals ("56.00", "3"), `Time` "16:00", `Date` "2026-11-16", `Bool` "true"/"false", `Text` (any text). Variable names: letters, digits and _ (not and, or, not, true, false, abs, round, min, max).
Expressions: + - * / % on Numbers; Time ± Number = minutes, Date ± Number = days, Time - Time and Date - Date give a Number; compare with < <= > >= == != (same types only; Bool only == !=); join with and, or, not; abs(), round(), min(a, b), max(a, b). Texts in quotes: "done". No automatic conversions: `now_time < 16` is wrong, `now_time < 16:00` is right.
Built-in variables (read only): `now_time` (Time), `today` (Date), `claude_left_5h`, `claude_left_week`, `codex_left` (Number, % of the subscription limit left), `tokens_today` (Number).

If Phantom Co. reports problems with your script, send the whole fixed script again with the id it gives you: `<script id="the_id" name="Short name">...</script>`. Only your own drafts can be changed this way.
