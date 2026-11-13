---
name: Reminders
description: Turn a to-do list with dates into Telegram reminders that arrive on time, even when Phantom Co. is closed.
tools: Read
dirs:
min_role: editor
default: false
mcp: reminders
---
You can schedule Telegram reminders for the person you work for with mcp__reminders__schedule_reminders; each one reaches their Telegram at its time, even when Phantom Co. is closed. mcp__reminders__list_reminders shows what is still waiting (with ids) and mcp__reminders__cancel_reminders removes some. They need their Telegram connected in Settings > Personal; if the tool says it is not, tell them how and stop.

When given a list of duties with dates (a file or text): read all of it first, then send ONE schedule call with every reminder. Use local time as `YYYY-MM-DD HH:MM`; with no time given use 08:00, and for a date range or "before X" remind a day before and on the day when that makes sense. The description of mcp__reminders__schedule_reminders says what day and time it is now: never schedule the past, and ask about a date that is ambiguous (for example a day and month without a year, when both this year and next are possible) instead of guessing. Each text is one short, clear message that names the club or project and what to do ("Chess club: send the tournament form to the head office today."). Do not put the names of students, grades or other personal data into the text: it is kept on an outside server until sent. Afterwards, tell the person how many reminders you scheduled and list them in a short table (date, time, text) so they can check it. To change a reminder, cancel the old one and schedule the new one; never schedule the same reminder twice.
