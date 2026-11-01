import { api } from '../api.js';
import { S, isOwner } from '../state.js';
import { $, esc, guard } from '../ui.js';

let view, scope = 'shared';

async function load(ctx) {
  const r = await api('GET', '/api/files?scope=' + encodeURIComponent(scope));
  if (!ctx.isCurrent()) return;
  const list = r.files.sort((a, b) => a.path.localeCompare(b.path));
  $('#tree', view).innerHTML = list.length ? list.map((f) => {
    const depth = f.path.split('/').length - 1, name = f.path.split('/').pop();
    return `<div class="f ${f.dir ? 'd' : ''}" style="padding-left:${12 + depth * 16}px" ${f.dir ? '' : `data-act="view" data-path="${esc(f.path)}"`}>${f.dir ? '📁' : '📄'} ${esc(name)}</div>`;
  }).join('') : '<div class="empty">Nothing here yet.<br>Ghosts will save their work here.</div>';
}

export async function render(v, arg, ctx) {
  view = v;
  scope = arg || scope;
  view.innerHTML = `<div class="page-head"><div><h1>Files</h1><p>What your ghosts produced. Shared files, or the folder of a project.</p></div><div class="spacer"></div>
    <select id="scope" style="width:auto"><option value="shared" ${scope === 'shared' ? 'selected' : ''}>Shared files</option>${S.state.projects.map((p) => `<option value="project:${esc(p.id)}" ${scope === 'project:' + p.id ? 'selected' : ''}>📁 ${esc(p.name)}</option>`).join('')}</select>
    <button class="btn" data-act="refresh">Refresh</button>${isOwner() ? '<button class="btn primary" data-act="vault">Open Vault in Explorer</button>' : ''}</div>
    <div class="files"><div class="tree" id="tree"></div><div class="viewer" id="viewer"><span style="color:var(--muted)">Pick a file to preview it.</span></div></div>`;
  await guard(() => load(ctx))();
  const q = () => '&scope=' + encodeURIComponent(scope);
  view.onclick = guard(async (e) => {
    const el = e.target.closest('[data-act]');
    if (!el) return;
    if (el.dataset.act === 'refresh') return load(ctx);
    if (el.dataset.act === 'vault') return api('POST', '/api/vault/open', {});
    if (el.dataset.act === 'view') {
      view.querySelectorAll('.tree .f.on').forEach((x) => x.classList.remove('on'));
      el.classList.add('on');
      const p = el.dataset.path, v2 = $('#viewer', view);
      if (/\.(png|jpe?g|gif|webp|svg)$/i.test(p)) { v2.innerHTML = `<img src="/api/files/raw?path=${encodeURIComponent(p)}${q()}" alt="">`; return; }
      const r = await api('GET', '/api/files/read?path=' + encodeURIComponent(p) + q());
      v2.innerHTML = r.binary ? '<span style="color:var(--muted)">Binary file — open it from Explorer.</span>' : `<pre>${esc(r.text)}</pre>${r.truncated ? '<p style="color:var(--muted)">… truncated</p>' : ''}`;
    }
  });
  view.onchange = (e) => { if (e.target.id === 'scope') location.hash = '#/files/' + encodeURIComponent(e.target.value); };
}
