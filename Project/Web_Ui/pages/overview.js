// Overview: what the ghosts used (tokens, reported cost, runs) and the spending limits. The owner sees the whole company
// and sets the limits; everybody else sees their own runs and their own limit.
import { api } from '../api.js';
import { lang } from '../i18n.js';
import { S, isOwner, agentById, userById, providerLabel } from '../state.js';
import { $, esc, guard, toast } from '../ui.js';

let view, data = null, days = 30;

const num = (n) => Number(n || 0).toLocaleString(lang === 'tr' ? 'tr-TR' : 'en-US');
const usd = (n) => '$' + Number(n || 0).toFixed(2);
const raw = (html) => `<span data-no-i18n>${html}</span>`;
const LIMITS = [['day_tokens', 'Tokens a day'], ['month_tokens', 'Tokens a month'], ['day_usd', '$ a day'], ['month_usd', '$ a month']];

function tile(label, value, sub = '') {
  return `<div class="ov-tile"><small>${label}</small><b data-no-i18n>${value}</b>${sub ? `<span class="muted small">${sub}</span>` : ''}</div>`;
}

// One bar per day: tokens. The whole height of a day is the hover target; the table of the rows below holds the same numbers.
function chartHTML() {
  const rows = data.by_day, top = Math.max(1, ...rows.map((r) => r.tokens));
  const label = (d) => new Date(d + 'T12:00:00').toLocaleDateString(lang === 'tr' ? 'tr-TR' : 'en-US', { day: 'numeric', month: 'short' });
  const every = Math.max(1, Math.ceil(rows.length / 8));
  return `<div class="ov-chart" id="ov-chart" role="img" aria-label="Tokens per day">
    <div class="ov-top muted small" data-no-i18n>${num(top)}</div>
    <div class="ov-bars">${rows.map((r, i) => `<div class="ov-day" data-date="${esc(label(r.date))}" data-tokens="${esc(num(r.tokens))}" data-cost="${esc(usd(r.cost))}" data-runs="${r.runs}">
        <i style="height:${r.tokens ? Math.max(1.5, (100 * r.tokens) / top) : 0}%"></i>${(rows.length - 1 - i) % every === 0 ? `<em data-no-i18n>${esc(label(r.date))}</em>` : ''}</div>`).join('')}</div>
    <div class="ov-tip" id="ov-tip" hidden data-no-i18n></div></div>`;
}

function tableHTML(title, rows, nameOf) {
  if (!rows.length) return '';
  return `<div class="card ov-card"><h3>${title}</h3><div class="ov-scroll"><table class="ov-table">
    <thead><tr><th></th><th>Tokens</th><th>From the cache</th><th>Cost</th><th>Runs</th><th>Failed</th></tr></thead>
    <tbody>${rows.map((r) => `<tr><td data-no-i18n>${esc(nameOf(r.id))}</td><td data-no-i18n>${num(r.tokens)}</td><td data-no-i18n>${r.tokens ? Math.round((100 * r.cached) / r.tokens) : 0}%</td><td data-no-i18n>${usd(r.cost)}</td><td data-no-i18n>${num(r.runs)}</td><td data-no-i18n>${num(r.failed)}</td></tr>`).join('')}</tbody></table></div></div>`;
}

const usedText = (key, used) => (key.endsWith('usd') ? usd(used[key]) : num(used[key]));
function barHTML(key, limit, used) {
  if (!limit[key]) return '';
  const pct = Math.min(100, (100 * used[key]) / limit[key]);
  return `<div class="ov-limit"><span class="muted small">${LIMITS.find(([k]) => k === key)[1]}</span> <span class="small" data-no-i18n>${usedText(key, used)} / ${key.endsWith('usd') ? usd(limit[key]) : num(limit[key])}</span>
    <div class="ov-meter ${pct >= 100 ? 'full' : ''}"><i style="width:${pct}%"></i></div></div>`;
}
function limitRow(scope, id, name, entry) {
  const l = entry.limit || {}, u = entry.used || {};
  return `<div class="ov-budget-row" data-scope="${scope}" data-id="${esc(id)}"><b data-no-i18n>${esc(name)}</b>
    ${LIMITS.map(([k, t]) => `<label><small>${t}</small><input type="number" min="0" step="${k.endsWith('usd') ? '0.01' : '1000'}" data-limit="${k}" value="${l[k] || ''}" placeholder="—"><span class="muted small" data-no-i18n>${usedText(k, u)}</span></label>`).join('')}</div>`;
}
function budgetsHTML() {
  const b = data.budgets;
  if (b.me) {
    const bars = LIMITS.map(([k]) => barHTML(k, b.me.limit || {}, b.me.used || {})).join('');
    return `<div class="card ov-card"><h3>Your limits</h3>${bars || '<p class="hint" style="margin-top:0">The owner has set no limit for you.</p>'}</div>`;
  }
  return `<div class="card ov-card" id="ov-budgets"><h3>Budgets</h3>
    <p class="hint" style="margin-top:0">When a limit is used up, new ghost work is refused until the day or month is over or you raise it. A run that is already going is finished. Empty = no limit. Tokens count as on the Runs page; dollars are what the tool reported (with a subscription, what the run would have cost). The small number shows the use so far.</p>
    <h3 class="subhead">Whole company</h3>${limitRow('company', '', S.state.company.name, b.company)}
    <h3 class="subhead">People</h3>${S.state.users.map((u) => limitRow('users', u.id, u.name, b.users[u.id] || {})).join('')}
    ${S.state.agents.length ? `<h3 class="subhead">Ghosts</h3>${S.state.agents.map((a) => limitRow('agents', a.id, a.name, b.agents[a.id] || {})).join('')}` : ''}
    <div class="modal-foot"><button class="btn primary" data-act="save-budgets">Save limits</button></div></div>`;
}

function draw() {
  const t = data.today, m = data.month;
  view.innerHTML = `<div class="page-head"><div><h1>Overview</h1><p>${isOwner() ? 'What the ghosts of the company used, and the limits you set for them.' : 'What your ghosts used.'}</p></div>
      <select id="ov-days" class="ov-days" aria-label="Period">${[7, 30, 90].map((d) => `<option value="${d}" ${d === days ? 'selected' : ''}>${d === 7 ? 'Last 7 days' : d === 30 ? 'Last 30 days' : 'Last 90 days'}</option>`).join('')}</select></div>
    <div class="ov-tiles">
      ${tile('Tokens today', num(t.tokens), raw(`${num(t.runs)} `) + '<span>runs</span>')}
      ${tile('Tokens this month', num(m.tokens), raw(`${num(m.runs)} `) + '<span>runs</span>')}
      ${tile('Cost this month', usd(m.cost))}
      ${tile('Failed this month', num(m.failed))}
    </div>
    <div class="card ov-card"><h3>Tokens per day</h3>${chartHTML()}</div>
    ${budgetsHTML()}
    ${tableHTML('By ghost', data.by_agent, (id) => agentById(id)?.name || id)}
    ${isOwner() ? tableHTML('By person', data.by_user, (id) => userById(id)?.name || id) : ''}
    ${tableHTML('By brain', data.by_brain, (id) => providerLabel(id))}`;

  const chart = $('#ov-chart', view), tip = $('#ov-tip', view);
  chart.onmousemove = (e) => {
    const day = e.target.closest('.ov-day');
    if (!day) { tip.hidden = true; return; }
    tip.innerHTML = `<b>${esc(day.dataset.date)}</b><br>${esc(day.dataset.tokens)} token · ${esc(day.dataset.cost)} · ${esc(day.dataset.runs)} run`;
    tip.hidden = false;
    const c = chart.getBoundingClientRect(), d = day.getBoundingClientRect();
    tip.style.left = Math.max(0, Math.min(c.width - tip.offsetWidth, d.left - c.left + d.width / 2 - tip.offsetWidth / 2)) + 'px';
  };
  chart.onmouseleave = () => { tip.hidden = true; };
}

async function load() { data = await api('GET', '/api/overview?days=' + days); }

export async function render(v) {
  view = v;
  await load();
  draw();
  view.onchange = guard(async (e) => {
    if (e.target.id !== 'ov-days') return;
    days = +e.target.value;
    await load();
    draw();
  });
  view.onclick = guard(async (e) => {
    if (!e.target.closest('[data-act="save-budgets"]')) return;
    const body = { company: {}, users: {}, agents: {} };
    view.querySelectorAll('.ov-budget-row').forEach((row) => {
      const l = {};
      row.querySelectorAll('[data-limit]').forEach((i) => { if (+i.value > 0) l[i.dataset.limit] = +i.value; });
      if (row.dataset.scope === 'company') body.company = l; else body[row.dataset.scope][row.dataset.id] = l;
    });
    await api('PUT', '/api/budgets', body);
    toast('Saved');
    await load();
    draw();
  });
}
