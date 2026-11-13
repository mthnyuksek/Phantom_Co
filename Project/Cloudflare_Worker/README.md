# Phantom Co. reminders (Cloudflare Worker)

A small Worker that keeps your reminders and sends them to Telegram at the right time, **also when your PC is off**.
Phantom Co.'s "Reminders (Cloudflare)" MCP server talks to it; ghosts schedule reminders, the Worker sends them.

It runs on Cloudflare's free plan: a free account (no card), one Worker, one D1 database, one cron trigger per minute.
Each person who wants this sets up their own Worker; nothing is shared between Phantom Co. users.

## Set it up (about 10 minutes)

You need [Node.js](https://nodejs.org) and a free [Cloudflare account](https://dash.cloudflare.com/sign-up). Open a terminal in this folder.

1. `npx wrangler login` (opens the browser once).
2. `npx wrangler d1 create phantom_reminders`: copy the `database_id` it prints into `wrangler.toml`.
3. `npx wrangler d1 execute phantom_reminders --remote --file=schema.sql`
4. The Telegram bot token (the bot from Settings > MCP > Telegram, or a new one from @BotFather):
   `npx wrangler secret put BOT_TOKEN`
5. A long random key (16+ characters; Phantom Co. proves itself with it):
   `npx wrangler secret put API_KEY`
6. `npx wrangler deploy`: it prints the Worker's address (`https://phantom-reminders.<you>.workers.dev`).

## Connect Phantom Co.

1. Settings > MCP > **Reminders (Cloudflare)**: paste the address and the key from step 5, save. "Connected" shows when it works.
2. Every person who wants reminders connects their Telegram in Settings > Personal (the Telegram MCP server must be on).
3. Give a ghost the **Reminders** skill. Hand it a list ("Chess club: tournament form due 2026-11-20 ...") and it schedules the Telegram messages.

A reminder goes to the Telegram of the person who asked the ghost, never to anybody else.

## What it stores

Only the reminder text, its time and the Telegram chat id, until it is sent (then the row is deleted). Do not put
personal data of students in reminders: it would be kept on Cloudflare until it is sent.

## Limits

- Reminders arrive within about a minute of their time. One that is more than 3 days late (Cloudflare down) is dropped.
- A failed send is tried again every minute, 5 times.
- At most 100 reminders per request, 5000 waiting, 500 characters each.
- Free plan: 100,000 requests a day and 5 million D1 rows read a day; the minute cron uses about 1,440 invocations a day.
- Change the key: `npx wrangler secret put API_KEY` again, then the same in Settings > MCP.
