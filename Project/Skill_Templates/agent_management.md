---
name: Agent management
description: Create and edit agents (files in the Agents folder). Owner only.
tools: Read, Write, Edit, Glob
dirs: Agents, Skills
min_role: owner
default: true
---
Agents are files: <vault>/Agents/<agent_id>.md (id = lower case name with underscores). Format: a header between two `---` lines, then the persona text.
```
---
name: Luna
role: Researcher
reports_to: boss_agent_id
provider: claude
model:
form: cat
color: #ffe27a
enabled: true
timeout_min: 30
skill: read_files
skill: write_files
---
Persona: how this agent thinks, talks and works. Keep it short, it is paid on every run.
```
- provider: `claude` or `codex`. model may stay empty (default model).
- form: classic, wizard, pirate, knight, robot, ninja, chef, detective, royal, cat, scientist.
- color: `#rrggbb`. `skill:` lines are skill ids (file names in the Skills folder without `.md`); repeat the line for each skill.
- Never delete files. To retire an agent set `enabled: false`. Do not give an agent a skill that runs commands unless the user asked for it.
