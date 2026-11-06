import { api, upload } from '../api.js';
import { S, isOwner, canEdit } from '../state.js';
import { $, esc, guard, toast, twoStep, modal, closeModal } from '../ui.js';
import { t } from '../i18n.js';

let view, scope = 'shared', folder = '';  // folder: where uploads go ('' = top of the scope)
// Folders start closed; a left click opens/closes one, a right click (long press on a phone) picks it as the
// place for uploads and new folders. Remembered per scope while the program runs.
const opened = new Map();  // scope -> Set of open folder paths
const openSet = () => { if (!opened.has(scope)) opened.set(scope, new Set()); return opened.get(scope); };
const parents = (path) => { const parts = path.split('/'), out = []; for (let i = 1; i < parts.length; i++) out.push(parts.slice(0, i).join('/')); return out; };
const openUpTo = (path) => { for (const p of [...parents(path), path]) if (p) openSet().add(p); };

const q = () => '&scope=' + encodeURIComponent(scope);
const size = (n) => n < 1024 ? n + ' B' : n < 1048576 ? (n / 1024).toFixed(1) + ' KB' : (n / 1048576).toFixed(1) + ' MB';

async function load(ctx) {
  const r = await api('GET', '/api/files?scope=' + encodeURIComponent(scope));
  if (!ctx.isCurrent()) return;
  const list = r.files.sort((a, b) => a.path.localeCompare(b.path));
  if (folder && !list.some((f) => f.dir && f.path === folder)) folder = '';  // the picked folder is gone
  const open = openSet(), count = {};
  for (const f of list) { const up = parents(f.path).pop(); if (up) count[up] = (count[up] || 0) + 1; }
  const shown = list.filter((f) => parents(f.path).every((p) => open.has(p)));
  $('#tree', view).innerHTML = list.length ? shown.map((f) => {
    const depth = f.path.split('/').length - 1, name = f.path.split('/').pop(), isOpen = open.has(f.path);
    if (f.dir) return `<div class="f d ${f.path === folder ? 'dest' : ''}" style="padding-left:${12 + depth * 16}px" data-act="toggle" data-path="${esc(f.path)}" title="Click: open or close · Right click: put new files here"><span class="caret">${isOpen ? '▾' : '▸'}</span> ${isOpen ? '📂' : '📁'} ${esc(name)} <span class="fmeta">${count[f.path] || 0}</span>${f.path === folder ? ' <span class="badge">⬆ here</span>' : ''}</div>`;
    return `<div class="f" style="padding-left:${12 + depth * 16 + 16}px" data-act="view" data-path="${esc(f.path)}">📄 ${esc(name)} <span class="fmeta">${size(f.size)}</span></div>`;
  }).join('') : '<div class="empty">Nothing here yet.<br>Ghosts save their work here, and you can upload files.</div>';
  $('#dest', view).textContent = folder ? folder + '/' : 'the top level';
  const reset = $('#dest-reset', view);
  if (reset) reset.hidden = !folder;
}

// 'yes' | 'no', and with more files to come also 'all' (replace the rest too) | 'none' (skip the rest too)
const askReplace = (name, more) => new Promise((resolve) => {
  modal(`<h2>Replace ${esc(name)}?</h2><p>A file with this name already exists.</p>
    <div class="row"><button class="btn primary" id="m-yes">Replace</button><button class="btn" id="m-no">Skip</button>
    ${more ? '<button class="btn" id="m-all">Replace all</button><button class="btn" id="m-none">Skip all</button>' : ''}</div>`, (el) => {
    for (const [id, answer] of [['m-yes', 'yes'], ['m-no', 'no'], ['m-all', 'all'], ['m-none', 'none']]) {
      const b = el.querySelector('#' + id);
      if (b) b.onclick = () => { closeModal(); resolve(answer); };
    }
  });
});

// items: [{ file, rel }] where rel is the path below the target folder ("a.txt" or "Folder/sub/a.txt" for a dropped folder)
async function sendFiles(items, ctx, into = folder) {
  let done = 0, replaceAll = null;  // null = ask, true / false = the answer for the rest
  for (let i = 0; i < items.length; i++) {
    const { file, rel } = items[i], path = (into ? into + '/' : '') + rel;
    try {
      await upload('/api/files/upload?path=' + encodeURIComponent(path) + q(), file);
      done++;
    } catch (e) {
      if (!/already exists/.test(e.message)) { toast(rel + ': ' + e.message, true); continue; }
      let answer = replaceAll === null ? await askReplace(rel, i < items.length - 1) : replaceAll ? 'yes' : 'no';
      if (answer === 'all' || answer === 'none') { replaceAll = answer === 'all'; answer = replaceAll ? 'yes' : 'no'; }
      if (answer === 'yes') { await upload('/api/files/upload?overwrite=1&path=' + encodeURIComponent(path) + q(), file); done++; }
    }
  }
  if (into) openUpTo(into);
  for (const { rel } of items) { const p = parents(rel); if (p.length) openUpTo((into ? into + '/' : '') + p[0]); }  // show a dropped folder open
  if (done) toast(t(done === 1 && items.length === 1 ? `Copied ${items[0].rel.split('/').pop()}` : `Copied ${done} files`));
  await load(ctx);
}

// Files and whole folders dropped from Explorer: folders are read entry by entry (webkitGetAsEntry), keeping their paths.
async function droppedItems(dt) {
  const entries = [...(dt.items || [])].map((it) => it.kind === 'file' && it.webkitGetAsEntry ? it.webkitGetAsEntry() : null);
  if (!entries.length || entries.some((e) => !e)) return [...dt.files].map((file) => ({ file, rel: file.name }));
  const out = [];
  const walk = async (entry, base) => {
    if (entry.isFile) { out.push({ file: await new Promise((ok, bad) => entry.file(ok, bad)), rel: base + entry.name }); return; }
    const reader = entry.createReader();
    for (;;) {  // readEntries returns at most 100 at a time
      const batch = await new Promise((ok, bad) => reader.readEntries(ok, bad));
      if (!batch.length) break;
      for (const e of batch) await walk(e, base + entry.name + '/');
    }
  };
  for (const e of entries) await walk(e, '');
  return out;
}
const hasFiles = (e) => [...(e.dataTransfer?.types || [])].includes('Files');
// Files dropped anywhere else in the window must not open in it (the app would navigate away to the file).
window.addEventListener('dragover', (e) => { if (hasFiles(e) && !e.defaultPrevented) { e.preventDefault(); e.dataTransfer.dropEffect = 'none'; } });
window.addEventListener('drop', (e) => { if (hasFiles(e)) e.preventDefault(); });

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
    ${editor ? '<p style="color:var(--muted);margin:0 0 8px">New files and folders go to <b id="dest">the top level</b> <button class="btn small" data-act="dest-top" id="dest-reset" hidden>× Top level</button> (right click a folder to change it, click a folder to open or close it).</p>' : '<span id="dest" hidden></span>'}
    <div class="files" id="drop"><div class="tree" id="tree"></div><div class="viewer" id="viewer"><span style="color:var(--muted)">Pick a file to preview it.</span></div>
      ${editor ? '<div class="drop-cover" id="drop-cover" hidden><div>⬇ <span id="drop-text"></span></div></div>' : ''}</div>`;
  await guard(() => load(ctx))();

  const pick = $('#pick', view);
  pick.onchange = guard(async () => { await sendFiles([...pick.files].map((file) => ({ file, rel: file.name })), ctx); pick.value = ''; });
  if (editor) {
    // Drag files or folders from Explorer anywhere onto the page; dropped on a folder row they go into that folder.
    const cover = $('#drop-cover', view);
    let depth = 0;
    const target = (e) => e.target.closest?.('.tree [data-act="toggle"]')?.dataset.path ?? folder;
    const show = (e) => {
      const into = target(e);
      view.querySelectorAll('.tree .f.drop-on').forEach((x) => x.classList.remove('drop-on'));
      e.target.closest?.('.tree [data-act="toggle"]')?.classList.add('drop-on');
      $('#drop-text', view).textContent = t('Drop to copy into') + ' ' + (into ? into + '/' : t('the top level'));
      cover.hidden = false;
    };
    const hide = () => { depth = 0; cover.hidden = true; view.querySelectorAll('.tree .f.drop-on').forEach((x) => x.classList.remove('drop-on')); };
    view.ondragenter = (e) => { if (hasFiles(e)) { depth++; show(e); } };
    view.ondragleave = (e) => { if (hasFiles(e) && --depth <= 0) hide(); };
    view.ondragover = (e) => { if (!hasFiles(e)) return; e.preventDefault(); e.dataTransfer.dropEffect = 'copy'; show(e); };
    view.ondrop = guard(async (e) => {
      if (!hasFiles(e)) return;
      e.preventDefault();
      const into = target(e);
      hide();
      const items = await droppedItems(e.dataTransfer);
      if (items.length) await sendFiles(items, ctx, into);
    });
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
          if (folder) openUpTo(folder);
          closeModal();
          await load(ctx);
        });
        m.querySelector('#m-go').onclick = go;
        m.querySelector('#m-name').onkeydown = (e) => { if (e.key === 'Enter') go(); };
      });
    }
    if (act === 'toggle') { const s = openSet(), path = el.dataset.path; if (s.has(path)) s.delete(path); else s.add(path); return load(ctx); }
    if (act === 'dest-top') { folder = ''; return load(ctx); }
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
  // right click on a folder: new files and folders go there (again: back to the top level)
  view.oncontextmenu = guard(async (e) => {
    const el = e.target.closest('.tree [data-act="toggle"]');
    if (!el || !editor) return;
    e.preventDefault();
    folder = folder === el.dataset.path ? '' : el.dataset.path;
    if (folder) openUpTo(folder);
    await load(ctx);
  });
  view.onchange = (e) => { if (e.target.id === 'scope') location.hash = '#/files/' + encodeURIComponent(e.target.value); };
}
