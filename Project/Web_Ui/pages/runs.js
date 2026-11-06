import { api } from '../api.js';
import { S, agentById, agentGhost, userById, canEdit, me, providerLabel } from '../state.js';
import { $, esc, guard, ago, sleep, pill, modal, duration, renderEvent, toast } from '../ui.js';

let view, runs = [];
// The share of the input read from the prompt cache (much cheaper than new input), for the list rows.
const cachedPart = (r) => (r.tokens_cached && r.tokens_in ? ` · ${Math.round((100 * r.tokens_cached) / r.tokens_in)}% cached` : '');

export async function openRunModal(id) {
  const r = await api('GET', '/api/runs/' + id);
  const a = agentById(r.agent_id), u = userById(r.user_id);
  let alive = true;
  modal(`<h2>Run · ${esc(a?.name || r.agent_id)} ${pill(r.status)}</h2>
    <p class="sub">${esc(r.kind)} · ${esc(providerLabel(r.provider))}${r.model ? ' · ' + esc(r.model) : ''}${u ? ' · for ' + esc(u.name) : ''} · ${esc(ago(r.created))}</p>
    <div class="runfoot" id="rm-foot"></div>
    <div class="transcript" id="rm-log" style="max-height:48vh"></div>
    <div class="modal-foot"><button class="btn" id="rm-close">Close</button></div>`,
  (m) => {
    const foot = (rec) => [rec.started ? '⏱ ' + esc(duration(rec.started, rec.ended)) : '', rec.tokens_in || rec.tokens_out ? '🔤 ' + (rec.tokens_in + rec.tokens_out).toLocaleString() + ' tokens' : '',
      rec.tokens_cached ? `<span title="Read again from the prompt cache: these cost about a tenth of new input.">♻️ ${rec.tokens_cached.toLocaleString()} from the cache</span>` : '',
      rec.cost ? '💲 ' + Number(rec.cost).toFixed(4) : '', rec.error ? '⚠️ ' + esc(rec.error) : ''].filter(Boolean).map((x) => `<span>${x}</span>`).join('');
    $('#rm-foot', m).innerHTML = foot(r);
    $('#rm-close', m).onclick = () => { alive = false; $('#modal-root').innerHTML = ''; };
    (async () => {
      let after = 0;
      while (alive) {
        try {
          const e = await api('GET', `/api/runs/${id}/events?after=${after}`);
          const log = $('#rm-log', m);
          if (!log) return;
          const near = log.scrollHeight - log.scrollTop - log.clientHeight < 80;
          log.insertAdjacentHTML('beforeend', e.events.map(renderEvent).join(''));
          if (near) log.scrollTop = log.scrollHeight;
          after = e.next ?? after;
          if (!['queued', 'running'].includes(e.status)) { const rec = await api('GET', '/api/runs/' + id); $('#rm-foot', m).innerHTML = foot(rec); return; }
        } catch { return; }
        await sleep(900);
      }
    })();
  }, true);
}

function draw() {
  const jobs = S.state.jobs;
  view.innerHTML = `<div class="page-head"><div><h1>Runs</h1><p>Every time a ghost worked. The record shows who asked, how long it took and what it cost.</p></div></div>
    ${jobs.length ? `<h3 class="section-title">Working right now</h3><div class="table">${jobs.map((j) => {
      const a = agentById(j.agent_id), u = userById(j.user_id);
      return `<div class="row job-row"><div class="mini">${agentGhost(a, { mood: j.running ? 'working' : 'queued', size: 34 })}</div>
        <div><div class="title">${esc(a?.name || j.agent_id)} <span class="meta">${esc(j.kind)}${u ? ' · for ' + esc(u.name) : ''}</span></div>
          <div class="meta">${j.running ? esc((j.activity || []).slice(-1)[0] || 'working…') : 'waiting in line'}</div></div>
        <div>${pill(j.running ? 'running' : 'queued')}</div><div class="meta hide-s">${j.started ? esc(duration(j.started)) : ''}</div>
        <div>${canEdit() && (j.user_id === me().id || me().role === 'owner') ? `<button class="btn small" data-act="cancel" data-id="${esc(j.id)}">Stop</button>` : ''}</div></div>`;
    }).join('')}</div>` : ''}
    <h3 class="section-title">History</h3>
    <div class="table">${runs.length ? runs.map((r) => {
      const a = agentById(r.agent_id), u = userById(r.user_id);
      return `<div class="row run-row" data-act="open" data-id="${esc(r.id)}"><div class="mini">${agentGhost(a, { size: 34 })}</div>
        <div><div class="title">${esc(a?.name || r.agent_id)} <span class="meta">${esc(r.kind)} · ${esc(providerLabel(r.provider))}${u ? ' · for ' + esc(u.name) : ''}</span></div>
          <div class="meta">${esc(ago(r.created))}${r.started ? ' · ' + esc(duration(r.started, r.ended)) : ''}</div></div>
        <div class="meta hide-s">${r.tokens_in || r.tokens_out ? (r.tokens_in + r.tokens_out).toLocaleString() + ' tokens' + cachedPart(r) : ''}</div><div>${pill(r.status)}</div><div class="hide-s"></div></div>`;
    }).join('') : '<div class="empty">Nothing has run yet.</div>'}</div>`;
}

export async function render(v) {
  view = v;
  runs = (await api('GET', '/api/runs')).runs;
  draw();
  view.onclick = guard(async (e) => {
    const el = e.target.closest('[data-act]');
    if (!el) return;
    if (el.dataset.act === 'open') await openRunModal(el.dataset.id);
    if (el.dataset.act === 'cancel') { await api('POST', `/api/runs/${el.dataset.id}/cancel`, {}); toast('Stopping…'); }
  });
}
export async function tick(changed) {
  const r = (await api('GET', '/api/runs')).runs;
  if (changed || JSON.stringify(r) !== JSON.stringify(runs)) { runs = r; draw(); }
}
