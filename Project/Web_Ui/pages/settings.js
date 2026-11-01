import { api } from '../api.js';
import { S, isOwner } from '../state.js';
import { $, esc, guard, toast } from '../ui.js';

let view, net = null;

function draw() {
  const s = S.state, c = s.company, set = s.settings;
  view.innerHTML = `
    <div class="page-head"><div><h1>Settings</h1><p>Your company, your Vault, sharing and limits. Only the owner sees this page.</p></div></div>
    <div class="card"><h3>Company</h3>
      <label>Name</label><input type="text" id="s-name" maxlength="60" value="${esc(c.name)}">
      <label>Goal <span class="hint-inline">every ghost reads this</span></label><textarea id="s-goal" maxlength="2000">${esc(c.goal)}</textarea>
      <div class="modal-foot"><button class="btn primary" data-act="save-company">Save</button></div></div>
    <div class="card"><h3>Sharing with friends</h3>
      <p class="hint" style="margin-top:0">Friends use their own Phantom Co. program and connect to this PC. Their ghost work runs here, with <b>your</b> Claude / Codex sign-in; nobody ever gets your login. It only works while this PC is on.</p>
      <label class="check"><input type="checkbox" id="s-share" ${s.share ? 'checked' : ''}> Let people on my network connect</label>
      ${s.share && net ? `<p class="hint">Friends connect to: ${net.addresses.map((a) => `<code>http://${esc(a.address)}:${net.port}</code> <span class="meta">(${esc(a.name)})</span>`).join(' &nbsp; ') || 'no network address found'}</p>` : ''}
      <p class="hint">Windows may ask once to allow the program through the firewall: allow it for <b>private</b> networks. The connection is not encrypted: use a home network or a private network app such as Tailscale, never the open internet. Sign-in is always required.</p></div>
    <div class="card"><h3>Limits</h3>
      <div class="two"><div><label>Runs at the same time (per brain)</label><input type="number" id="s-conc" min="1" max="4" value="${set.max_concurrent}"></div>
        <div><label>Waiting runs per person</label><input type="number" id="s-queue" min="0" max="100" value="${set.max_queue_per_user}"></div></div>
      <div class="two"><div><label>Hand-overs between ghosts (safety net)</label><input type="number" id="s-hops" min="1" max="20" value="${set.max_hops}"></div>
        <div><label>Default Claude model <span class="hint-inline">empty = Claude Code default</span></label><input type="text" id="s-model" maxlength="80" value="${esc(set.default_model)}" placeholder="e.g. haiku"></div></div>
      <div class="modal-foot"><button class="btn primary" data-act="save-limits">Save</button></div></div>
    <div class="card"><h3>Vault folder</h3>
      <p class="hint" style="margin-top:0">Everything lives here as plain files: people, ghosts, skills, rules, projects, chats, runs. Pointing to a new empty folder creates a fresh Vault there (old files are not moved).</p>
      <div class="inline"><input type="text" id="s-vault" value="${esc(s.vault_path)}"><button class="btn" data-act="save-vault">Use this folder</button><button class="btn" data-act="open-vault">Open</button></div></div>
    <div class="card"><h3>Brains on this PC</h3>
      ${s.providers.map((p) => `<div style="display:flex;gap:10px;align-items:center;padding:4px 0"><span class="chip ${p.available ? 'ok' : ''}">${p.available ? 'Ready' : 'Not found'}</span> ${esc(p.label)}</div>`).join('')}
      <p class="hint">Claude and Codex are used through their own command-line tools, signed in on this PC.</p></div>`;
}

export async function render(v) {
  view = v;
  if (!isOwner()) { view.innerHTML = '<div class="empty">Only the owner can change settings.</div>'; return; }
  net = await api('GET', '/api/network').catch(() => null);
  draw();
  view.onclick = guard(async (e) => {
    const el = e.target.closest('[data-act]');
    if (!el) return;
    const act = el.dataset.act;
    if (act === 'save-company') { await api('PUT', '/api/company', { name: $('#s-name', view).value, goal: $('#s-goal', view).value }); toast('Saved'); window.dispatchEvent(new Event('pc2:changed')); }
    else if (act === 'save-limits') {
      await api('PUT', '/api/settings', { max_concurrent: +$('#s-conc', view).value, max_queue_per_user: +$('#s-queue', view).value, max_hops: +$('#s-hops', view).value, default_model: $('#s-model', view).value.trim() });
      toast('Saved'); window.dispatchEvent(new Event('pc2:changed'));
    } else if (act === 'save-vault') { await api('PUT', '/api/settings/vault', { path: $('#s-vault', view).value.trim() }); toast('Vault changed'); window.dispatchEvent(new Event('pc2:changed')); }
    else if (act === 'open-vault') await api('POST', '/api/vault/open', {});
  });
  view.onchange = guard(async (e) => {
    if (e.target.id !== 's-share') return;
    net = await api('PUT', '/api/network', { share: e.target.checked });
    toast(e.target.checked ? 'Sharing is on' : 'Sharing is off');
    await new Promise((r) => setTimeout(r, 900));  // the Node restarts its listener
    window.dispatchEvent(new Event('pc2:changed'));
  });
}
