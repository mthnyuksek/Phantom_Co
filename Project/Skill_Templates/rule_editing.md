---
name: Rule editing
description: Create and edit company rules (files in the Rules folder). Owner only.
tools: Read, Write, Edit, Glob
dirs: Rules
min_role: owner
default: false
---
Company rules are files: <vault>/Rules/<rule_id>.md. Every enabled rule is added to the prompt of every agent on every run, so keep rules few and short.
```
---
name: Short title
enabled: true
---
The rule text.
```
Never delete files; set `enabled: false` to switch a rule off.
