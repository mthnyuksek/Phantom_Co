// Phantom Co. reminders: a Cloudflare Worker that keeps reminders in a D1 database and sends them to Telegram
// at the time, also when the PC with Phantom Co. is off. Setup: README.md next to this file.
//
// Phantom Co. calls (header `Authorization: Bearer <API_KEY>`):
//   POST /reminders          {chat_id, reminders: [{at: unix seconds, text}]}   -> {ids}
//   GET  /reminders?chat=ID  the reminders of one chat that are still waiting   -> {reminders: [{id, at, text}]}
//   POST /reminders/cancel   {chat_id, ids: [..]}                               -> {removed}
//   GET  /health                                                                -> {ok, waiting, telegram}
// A cron trigger (every minute) sends what is due. A sent reminder is deleted at once (no text stays behind).

const MAX_TEXT = 500;
const MAX_BATCH = 100;
const MAX_WAITING = 5000;      // a stray key cannot fill the database
const MAX_TRIES = 5;           // Telegram refused or was down: tried again each minute, then dropped
const MAX_LATE = 3 * 86400;    // a reminder that is more than 3 days late is dropped instead of sent

const json = (body, status = 200) => new Response(JSON.stringify(body), { status, headers: { 'content-type': 'application/json' } });

function sameText(a, b) {  // constant time for equal lengths; the key is long and random
  if (a.length !== b.length) return false;
  let d = 0;
  for (let i = 0; i < a.length; i++) d |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return d === 0;
}

async function sendTelegram(env, chat, text) {
  try {
    const r = await fetch(`https://api.telegram.org/bot${String(env.BOT_TOKEN).trim()}/sendMessage`, {
      method: 'POST', headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ chat_id: chat, text: `⏰ ${text}` }),
    });
    return r.ok;
  } catch { return false; }
}

async function sendDue(env) {
  const now = Math.floor(Date.now() / 1000);
  await env.DB.prepare('DELETE FROM reminders WHERE at < ? OR tries >= ?').bind(now - MAX_LATE, MAX_TRIES).run();
  const due = await env.DB.prepare('SELECT id, chat_id, text FROM reminders WHERE at <= ? ORDER BY at LIMIT 50').bind(now).all();
  for (const row of due.results) {
    if (await sendTelegram(env, row.chat_id, row.text)) await env.DB.prepare('DELETE FROM reminders WHERE id = ?').bind(row.id).run();
    else await env.DB.prepare('UPDATE reminders SET tries = tries + 1 WHERE id = ?').bind(row.id).run();
  }
}

async function handle(request, env) {
  const apiKey = String(env.API_KEY || '').trim();  // a pasted secret often carries a line break
  if (apiKey.length < 16) return json({ error: 'API_KEY secret is missing or too short (16+ characters)' }, 500);
  const given = (request.headers.get('authorization') || '').replace(/^Bearer /, '').trim();
  if (!sameText(given, apiKey)) return json({ error: 'wrong key' }, 401);
  const url = new URL(request.url);

  if (request.method === 'GET' && url.pathname === '/health') {
    const n = await env.DB.prepare('SELECT COUNT(*) AS n FROM reminders').first('n');
    return json({ ok: true, waiting: n, telegram: !!env.BOT_TOKEN });
  }
  if (request.method === 'GET' && url.pathname === '/reminders') {
    const chat = url.searchParams.get('chat') || '';
    if (!/^-?\d{1,20}$/.test(chat)) return json({ error: 'chat must be a number' }, 400);
    const rows = await env.DB.prepare('SELECT id, at, text FROM reminders WHERE chat_id = ? ORDER BY at LIMIT 500').bind(chat).all();
    return json({ reminders: rows.results });
  }
  if (request.method === 'POST' && (url.pathname === '/reminders' || url.pathname === '/reminders/cancel')) {
    let b;
    try { b = await request.json(); } catch { return json({ error: 'bad JSON' }, 400); }
    const chat = String(b.chat_id ?? '');
    if (!/^-?\d{1,20}$/.test(chat)) return json({ error: 'chat_id must be a number' }, 400);

    if (url.pathname === '/reminders/cancel') {
      const ids = (Array.isArray(b.ids) ? b.ids : []).filter(Number.isInteger).slice(0, 500);
      let removed = 0;
      for (const id of ids) removed += (await env.DB.prepare('DELETE FROM reminders WHERE id = ? AND chat_id = ?').bind(id, chat).run()).meta.changes;
      return json({ removed });
    }
    const list = Array.isArray(b.reminders) ? b.reminders : [];
    if (!list.length || list.length > MAX_BATCH) return json({ error: `send 1 to ${MAX_BATCH} reminders` }, 400);
    for (const r of list)
      if (!Number.isInteger(r.at) || r.at < 1e9 || r.at > 4.2e9 || typeof r.text !== 'string' || !r.text.trim()) return json({ error: 'each reminder needs at (unix seconds) and text' }, 400);
    const waiting = await env.DB.prepare('SELECT COUNT(*) AS n FROM reminders').first('n');
    if (waiting + list.length > MAX_WAITING) return json({ error: 'too many reminders waiting' }, 429);
    const now = Math.floor(Date.now() / 1000);
    const ids = [];
    for (const r of list) {
      const res = await env.DB.prepare('INSERT INTO reminders (chat_id, at, text, created) VALUES (?, ?, ?, ?)').bind(chat, r.at, r.text.trim().slice(0, MAX_TEXT), now).run();
      ids.push(res.meta.last_row_id);
    }
    return json({ ids });
  }
  return json({ error: 'not found' }, 404);
}

export default {
  async fetch(request, env) {
    try { return await handle(request, env); } catch (e) { return json({ error: 'server error' }, 500); }
  },
  async scheduled(event, env, ctx) {
    if (env.BOT_TOKEN) ctx.waitUntil(sendDue(env));
  },
};
