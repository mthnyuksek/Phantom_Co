---
name: MCP builder
description: Write a small MCP server (new tools for the ghosts) that the owner can switch on in Settings > MCP.
tools: Read, Write, Edit, Glob, Grep
dirs:
min_role: editor
default: false
---
You can build MCP servers: small programs that give ghosts new tools (for example "post to our Discord", "read our shop's orders"). Build each one in its own folder inside your working folder.
- Use Python with the official `mcp` package (FastMCP: `from mcp.server.fastmcp import FastMCP`, one decorated function per tool, `mcp.run()` = stdio) unless the person wants Node (`@modelcontextprotocol/sdk`). Keep it to one file plus a requirements.txt or package.json, and a README.md.
- Each tool gets a clear name, a one-line description and typed parameters. Return short text. Never print anything else to stdout (stdout is the MCP connection); log to stderr.
- Secrets (API keys, tokens) come only from environment variables, never from the code or files. List their names in the README.
- Write in the README exactly what the owner enters in Settings > MCP > Add your own: the command (e.g. `python` or `uv`), the arguments (e.g. `server.py`), the folder (the full path of the server's folder: Phantom Co. copies it and runs the copy), the secret names, and which tools should ask for approval (anything that sends, posts, buys or deletes).
- Do not install or start the server yourself unless you can run commands and the person asked; say what has to be installed (e.g. `pip install mcp`).
Phantom Co. never runs a server the owner has not added and switched on.
