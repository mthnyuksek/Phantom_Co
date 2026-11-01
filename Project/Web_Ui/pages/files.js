import { api, upload } from '../api.js';
import { S, isOwner, canEdit } from '../state.js';
import { $, esc, guard, toast, twoStep, modal, closeModal } from '../ui.js';

let view, scope = 'shared', folder = '';  // folder: where uploads go ('' = top of the scope)

const q = () => '&scope=' + encodeURIComponent(scope);
const size = (n) => n < 1024 ? n + ' B' : n < 1048576 ? (n / 1024).toFixed(1) + ' KB' : (n / 1048576).toFixed(1) + ' MB';

async function load(ctx) {
  const r = await api('GET', '/api/files?scope=' + encodeURIComponent(scope));
  if (!ctx.isCurrent()) return;
  const list = r.files.sort((a, b) => a.path.localeCompare(b.path));
  $('#tree', view).innerHTML = list.length ? list.map((f) => {
    const depth = f.path.split('/').length - 1, name = f.path.split('/').pop();
    return `<div class="f ${f.dir ? 'd' : ''} ${f.dir && f.path === folder ? 'on' : ''}" style="padding-left:${12 + depth * 16}px" data-act="${f.dir ? 'pick-folder' : 'view'}" data-path="${esc(f.path)}">${f.dir ? '📁' : '📄'} ${esc(name)}${f.dir ? '' : ` <span style="color:var(--muted);font-size:11px">${size(f.size)}</span>`}</div>`;
  }).join('') : '<div class="empty">Nothing here yet.<br>Ghosts save their work here, and you can upload files.</div>';
  $('#dest', view).textContent = folder ? folder + '/' : 'the top level';
}

const askReplace = (name) => new Promise((resolve) => {
  modal(`<h2>Replace ${esc(name)}?</h2><p>A file with this name already exists.</p>
    <div class="row"><button class="btn primary" id="m-yes">Replace</button><button class="btn" id="m-no">Skip</button></div>`, (el) => {
    el.querySelector('#m-yes').onclick = () => { closeModal(); resolve(true); };
    el.querySelector('#m-no').onclick = () => { closeModal(); resolve(false); };
  });
});

async function sendFiles(files, ctx) {
  for (const f of files) {
    const path = (folder ? folder + '/' : '') + f.name;
    try {
      await upload('/api/files/upload?path=' + encodeURIComponent(path) + q(), f);
      toast('Uploaded ' + f.name);
    } catch (e) {
      if (/already exists/.test(e.message)) {
        if (await askReplace(f.name)) { await upload('/api/files/upload?overwrite=1&path=' + encodeURIComponent(path) + q(), f); toast('Replaced ' + f.name); }
      } else toast(f.name + ': ' + e.message, true);
    }
  }
  await load(ctx);
}

export async function render(v, arg, ctx) {
  view = v;
  if (arg && arg !== scope) folder = '';
  scope = arg || scope;
  const editor = canEdit();
  view.innerHTML = `<div class="page-head"><div><h1>Files</h1><p>What your ghosts produced. Shared files, or the folder of a project. ${editor ? 'Upload files to hand them to your ghosts and teammates.' : ''}</p></div><div class="spacer"></div>
    <select id="scope" style="width:auto"><option value="shared" ${scope === 'shared' ? 'selected' : ''}>Shared files</option>${S.state.projects.map((p) => `<option value="project:${esc(p.id)}" ${scope === 'project:' + p.id ? 'selected' : ''}>📁 ${esc(p.name)}</option>`).join('')}</select>
    <button class="btn" data-act="refresh">Refresh</button>
    ${editor ? '<button class="btn primary" data-act="upload">⬆ Upload</button><button class="btn" data-act="mkdir">New folder</button>' : ''}
    ${isOwner() ? '<button class="btn" data-act="vault">Open Vault in Explorer</button>' : ''}<input type="file" id="pick" multiple hidden></div>
    ${editor ? '<p style="color:var(--muted);margin:0 0 8px">Uploads go to <b id="dest">the top level</b> (click a folder to change it).</p>' : '<span id="dest" hidden></span>'}
    <div class="files" id="drop"><div class="tree" id="tree"></div><div class="viewer" id="viewer"><span style="color:var(--muted)">Pick a file to preview it.</span></div></div>`;
  await guard(() => load(ctx))();

  const pick = $('#pick', view), drop = $('#drop', view);
  pick.onchange = guard(async () => { await sendFiles([...pick.files], ctx); pick.value = ''; });
  if (editor) {
    drop.ondragover = (e) => { e.preventDefault(); };
    drop.ondrop = guard(async (e) => { e.preventDefault(); await sendFiles([...e.dataTransfer.files], ctx); });
  }

  view.onclick = guard(async (e) => {
    const el = e.target.closest('[data-act]');
    if (!el) return;
    const act = el.dataset.act;
    if (act === 'refresh') return load(ctx);
    if (act === 'vault') return api('POST', '/api/vault/open', {});
    if (act === 'upload') return pick.click();
    if (act === 'mkdir') {
      return modal(`<h2>New folder</h2><input id="m-name" type="text" maxlength="80" placeholder="Folder name" autofocus>
        <div class="row"><button class="btn primary" id="m-go">Create</button></div>`, (m) => {
        const go = guard(async () => {
          const name = m.querySelector('#m-name').value.trim();
          if (!name) return;
          await api('POST', '/api/files/mkdir', { scope, path: (folder ? folder + '/' : '') + name });
          closeModal();
          await load(ctx);
        });
        m.querySelector('#m-go').onclick = go;
        m.querySelector('#m-name').onkeydown = (e) => { if (e.key === 'Enter') go(); };
      });
    }
    if (act === 'pick-folder') { folder = folder === el.dataset.path ? '' : el.dataset.path; return load(ctx); }
    if (act === 'delete') {
      return twoStep(el, 'Really?', async () => {
        await api('DELETE', '/api/files?path=' + encodeURIComponent(el.dataset.path) + q());
        $('#viewer', view).innerHTML = '<span style="color:var(--muted)">Deleted.</span>';
        await load(ctx);
      });
    }
    if (act === 'view') {
      view.querySelectorAll('.tree .f.on').forEach((x) => x.classList.remove('on'));
      el.classList.add('on');
      const p = el.dataset.path, v2 = $('#viewer', view);
      const bar = `<p><a class="btn small" href="/api/files/download?path=${encodeURIComponent(p)}${q()}" download>⬇ Download</a>
        ${isOwner() ? `<button class="btn small danger" data-act="delete" data-path="${esc(p)}">Delete</button>` : ''}</p>`;
      if (/\.(png|jpe?g|gif|webp|svg)$/i.test(p)) { v2.innerHTML = bar + `<img src="/api/files/raw?path=${encodeURIComponent(p)}${q()}" alt="">`; return; }
      const r = await api('GET', '/api/files/read?path=' + encodeURIComponent(p) + q());
      v2.innerHTML = bar + (r.binary ? '<span style="color:var(--muted)">Binary file — download it to open it.</span>' : `<pre>${esc(r.text)}</pre>${r.truncated ? '<p style="color:var(--muted)">… truncated</p>' : ''}`);
    }
  });
  view.onchange = (e) => { if (e.target.id === 'scope') location.hash = '#/files/' + encodeURIComponent(e.target.value); };
}
