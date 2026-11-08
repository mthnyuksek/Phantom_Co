// Search: chats, tasks, project notes and the errors of runs, whatever this person may read. Capital letters and Turkish
// marks do not matter on the server's side (i = ı = İ, s = ş...). The text is kept in the address, so "back" returns to the results.
import { api } from '../api.js';
import { $, esc, ago, guard } from '../ui.js';

const TYPE = { chat: ['💬', 'Chat'], agents: ['👻', 'Ghosts talking'], task_chat: ['📋', 'Task conversation'], script_chat: ['🧩', 'Script conversation'],
  task: ['📋', 'Task'], project: ['📁', 'Project'], note: ['📝', 'Project note'], run: ['⏱️', 'Run error'] };

let view, timer = 0, asked = '';

function rowHTML(r) {
  const [icon, label] = TYPE[r.type] || ['🔎', r.type];
  return `<a class="sr-row" href="${esc(r.link)}"><i>${icon}</i><div class="sr-body">
    <div class="sr-title"><b data-no-i18n>${esc(r.title)}</b> <span class="badge">${esc(label)}</span> <span class="muted small">${esc(ago(r.ts))}</span></div>
    <div class="sr-snip" data-no-i18n>${esc(r.before)}<mark>${esc(r.match)}</mark>${esc(r.after)}</div></div></a>`;
}

const find = guard(async (q) => {
  asked = q;
  const box = $('#sr-results', view);
  if (q.trim().length < 2) { box.innerHTML = '<div class="empty small">Write at least two letters.</div>'; return; }
  box.innerHTML = '<div class="empty small">Searching…</div>';
  const r = await api('GET', '/api/search?q=' + encodeURIComponent(q.trim()));
  if (q !== asked || !view.contains(box)) return;  // a newer search is on its way
  box.innerHTML = r.results.map(rowHTML).join('') || '<div class="empty small">Nothing found.</div>';
  if (r.truncated) box.insertAdjacentHTML('beforeend', '<p class="hint">Only the newest results are shown. Search for something more exact to see the rest.</p>');
});

export async function render(v, arg) {
  view = v;
  view.innerHTML = `<div class="page-head"><div><h1>Search</h1><p>Chats, tasks, project notes and the errors of runs. Only what you may read.</p></div></div>
    <input type="text" id="sr-q" placeholder="Search…" value="${esc(arg || '')}" autocomplete="off" aria-label="Search">
    <div id="sr-results" class="sr-results"></div>`;
  const input = $('#sr-q', view);
  input.focus();
  input.setSelectionRange(input.value.length, input.value.length);
  input.oninput = () => {
    clearTimeout(timer);
    timer = setTimeout(() => {
      history.replaceState(null, '', '#/search/' + encodeURIComponent(input.value));
      find(input.value);
    }, 300);
  };
  find(input.value);
}
