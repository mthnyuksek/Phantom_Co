// Project/Cloudflare_Worker/worker.js against an in-memory SQLite standing in for D1 and a fake Telegram (fetch).
// Usage: node reminders_worker_test.mjs   (Node 22.13+ or 24: node:sqlite)
import { DatabaseSync } from 'node:sqlite';
import { readFileSync } from 'node:fs';
import { fileURLToPath, pathToFileURL } from 'node:url';
import path from 'node:path';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '..', '..', 'Cloudflare_Worker');
const worker = (await import(pathToFileURL(path.join(root, 'worker.js')).href)).default;

const failures = [];
const check = (name, cond, extra = '') => { if (!cond) { failures.push(name); console.log('FAIL', name, extra); } };

const db = new DatabaseSync(':memory:');
db.exec(readFileSync(path.join(root, 'schema.sql'), 'utf8'));

// the part of D1's API the Worker uses
const D1 = {
  prepare(sql) {
    const st = db.prepare(sql);
    let args = [];
    const o = {
      bind(...a) { args = a; return o; },
      async run() { const r = st.run(...args); return { meta: { changes: Number(r.changes), last_row_id: Number(r.lastInsertRowid) } }; },
      async all() { return { results: st.all(...args).map((x) => ({ ...x })) }; },
      async first(col) { const r = st.get(...args); return r ? (col ? r[col] : { ...r }) : null; },
    };
    return o;
  },
};

const sent = [];
let telegramUp = true;
globalThis.fetch = async (url, init) => {
  const body = JSON.parse(init.body);
  if (!telegramUp) return { ok: false };
  sent.push({ url, chat: body.chat_id, text: body.text });
  return { ok: true };
};

const KEY = 'k'.repeat(20);
const env = { DB: D1, API_KEY: KEY, BOT_TOKEN: 'T0KEN' };
const call = async (method, p, body, key = KEY, e = env) => {
  const res = await worker.fetch(new Request('https://w.example' + p, {
    method, headers: { authorization: 'Bearer ' + key, 'content-type': 'application/json' }, body: body ? JSON.stringify(body) : undefined,
  }), e);
  return { status: res.status, body: await res.json() };
};
const now = () => Math.floor(Date.now() / 1000);
const cron = async (e = env) => { const ps = []; await worker.scheduled({}, e, { waitUntil: (p) => ps.push(p) }); await Promise.all(ps); };

// auth
let r = await call('GET', '/health', null, 'wrong');
check('wrong key refused', r.status === 401, r);
r = await call('GET', '/health', null, KEY, { ...env, API_KEY: 'short' });
check('short key refused', r.status === 500, r);
r = await call('GET', '/health', null, KEY, { ...env, API_KEY: KEY + '\r\n', BOT_TOKEN: 'T0KEN\r\n' });
check('secret with a trailing line break still works', r.status === 200, r);
r = await call('GET', '/health');
check('health',r.status === 200 && r.body.ok && r.body.waiting === 0 && r.body.telegram === true, r);
r = await call('GET', '/health', null, KEY, { ...env, BOT_TOKEN: undefined });
check('health without bot token', r.body.telegram === false, r);

// add
r = await call('POST', '/reminders', { chat_id: '555', reminders: [{ at: now() - 10, text: 'due now' }, { at: now() + 3600, text: 'later' }] });
check('add', r.status === 200 && r.body.ids.length === 2, r);
await call('POST', '/reminders', { chat_id: '777', reminders: [{ at: now() + 60, text: 'other person' }] });
r = await call('POST', '/reminders', { chat_id: '555', reminders: [{ at: 'soon', text: 'x' }] });
check('bad time refused', r.status === 400, r);
r = await call('POST', '/reminders', { chat_id: 'abc', reminders: [{ at: now() + 60, text: 'x' }] });
check('bad chat refused', r.status === 400, r);
r = await call('POST', '/reminders', { chat_id: '555', reminders: [] });
check('empty refused', r.status === 400, r);
r = await call('POST', '/reminders', { chat_id: '555', reminders: [{ at: now() + 60, text: 'x'.repeat(900) }] });
check('add long', r.status === 200, r);
r = await call('GET', '/reminders?chat=555');
check('list own chat only', r.body.reminders.length === 3 && r.body.reminders.every((x) => x.text !== 'other person'), r);
check('long text cut', r.body.reminders.some((x) => x.text.length === 500), r);

// cron: sends what is due, deletes it
await cron();
check('due one sent', sent.length === 1 && sent[0].chat === '555' && sent[0].text === '⏰ due now' && sent[0].url.includes('botT0KEN'), sent);
r = await call('GET', '/reminders?chat=555');
check('sent one deleted', r.body.reminders.length === 2 && r.body.reminders.every((x) => x.text !== 'due now'), r);
await cron();
check('not sent twice', sent.length === 1, sent);

// cron without a bot token does nothing
await call('POST', '/reminders', { chat_id: '555', reminders: [{ at: now() - 5, text: 'no token' }] });
await cron({ ...env, BOT_TOKEN: '' });
check('no token no send', sent.length === 1, sent);

// Telegram down: kept, tried again, dropped after 5 tries
telegramUp = false;
for (let i = 0; i < 4; i++) await cron();
r = await call('GET', '/reminders?chat=555');
check('kept while Telegram is down', r.body.reminders.some((x) => x.text === 'no token'), r);
telegramUp = true;
await cron();
check('sent when Telegram is back', sent.length === 2 && sent[1].text === '⏰ no token', sent);
telegramUp = false;
await call('POST', '/reminders', { chat_id: '555', reminders: [{ at: now() - 5, text: 'doomed' }] });
for (let i = 0; i < 6; i++) await cron();
r = await call('GET', '/reminders?chat=555');
check('dropped after 5 tries', !r.body.reminders.some((x) => x.text === 'doomed'), r);
telegramUp = true;

// very late ones are dropped, not sent
db.exec(`INSERT INTO reminders (chat_id, at, text, created) VALUES ('555', ${now() - 5 * 86400}, 'ancient', 0)`);
const before = sent.length;
await cron();
check('ancient dropped', sent.length === before && db.prepare("SELECT COUNT(*) AS n FROM reminders WHERE text = 'ancient'").get().n === 0, sent);

// cancel: only the own chat's rows
const list = (await call('GET', '/reminders?chat=555')).body.reminders;
const other = (await call('GET', '/reminders?chat=777')).body.reminders;
r = await call('POST', '/reminders/cancel', { chat_id: '555', ids: [list[0].id, other[0].id, 'x'] });
check('cancel own only', r.body.removed === 1, r);
r = await call('GET', '/reminders?chat=777');
check('other chat untouched', r.body.reminders.length === 1, r);

// limits
r = await call('POST', '/reminders', { chat_id: '555', reminders: Array.from({ length: 101 }, () => ({ at: now() + 60, text: 'x' })) });
check('batch limit', r.status === 400, r);
r = await call('GET', '/nowhere');
check('404', r.status === 404, r);

console.log(failures.length ? 'FAILED: ' + failures.join(', ') : 'all worker checks passed');
process.exit(failures.length ? 1 : 0);
