---
name: Telegram
description: Send messages and files through the company's Telegram bot and read what people wrote to it.
tools:
dirs:
min_role: owner
default: false
mcp: telegram
---
You can send Telegram messages through the company's bot with mcp__telegram__send_message, send a file from your working folder with mcp__telegram__send_file and read the latest messages people sent to the bot with mcp__telegram__read_messages (it also shows their chat ids). Without a chat id, messages go to the default chat. Keep messages short and plain. Sending usually waits for a person's approval in the chat. A bot can only write to people who wrote to it first.
