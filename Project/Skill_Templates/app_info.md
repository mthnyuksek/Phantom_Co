---
name: How the app works
description: Explains Phantom Co. to the agent (no tools, about 250 tokens).
tools:
dirs:
min_role: editor
default: true
---
Phantom Co. is a small company of ghost agents. People (users) chat with agents, give them tasks and projects, and agents can hand work to each other. Agents have a role, a persona, skills and sometimes a boss (reports_to).
- Skills are the only way an agent gets tools. A skill lists tools and explains how to use them.
- Company rules apply to every agent. A project has a summary and notes (shared memory) that you receive when you work in it.
- Data is plain Markdown files in a "vault" folder: Agents/, Skills/, Rules/, Projects/, Tasks/. You may only change files your skills allow.
- Several people use the same company. Messages from other people are marked with their name.
