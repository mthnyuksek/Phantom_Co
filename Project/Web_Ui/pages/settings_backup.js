// Two cards of Settings: which notifications go out, and the backups of the Vault.
import { api } from '../api.js';
import { $, esc, guard, toast, twoStep } from '../ui.js';

const size = (n) => (n >= 1073741824 ? (n / 1073741824).toFixed(1) + ' GB' : n >= 1048576 ? (n / 1048576).toFixed(1) + ' MB' : Math.max(1, Math.round(n / 1024)) + ' KB');
const dateOf = (name) => { const m = name.match(/(\d{4}-\d{2}-\d{2})_(\d{2})(\d{2})/); return m ? `${m[1]} ${m[2]}:${m[3]}` : name; };

export const careCardsHTML = () => `<div class="card" id="s-notify"><h3>Notifications</h3><p class="hint" style="margin-top:0">Loading…</p></div>
  <div class="card" id="s-backup"><h3>Backup</h3><p class="hint" style="margin-top:0">Loading…</p></div>`;

const box = (id, on, label) => `<label class="check"><input type="checkbox" data-n="${id}" ${on ? 'checked' : ''}> ${label}</label>`;
function notifyHTML(s) {
  return `<h3>Notifications</h3>
    <p class="hint" style="margin-top:0">The bell in the sidebar always shows them to the person they are for. They can also come as:</p>
    ${box('windows', s.windows, 'A Windows notification on this PC')}
    ${box('telegram', s.telegram, 'A Telegram message (for people who connected their Telegram in the profile)')}
    <h3 class="subhead">Tell me when</h3>
    ${box('failed', s.failed, 'A script, a task or a wake-up failed')}
    ${box('ask', s.ask, 'A ghost asks for approval or a script asks a question')}
    ${box('budget', s.budget, 'A budget is used up')}
    ${box('done', s.done, 'Long work is finished')}
    <label>Long means at least (minutes)</label><input type="number" min="0" max="1440" data-n="done_min" value="${s.done_min}" style="max-width:120px">`;
}

function backupHTML(b, keepChecked) {
  const running = b.running;
  return `<h3>Backup</h3>
    <p class="hint" style="margin-top:0">One zip file with your Vault: ghosts, skills, chats, tasks, runs, project notes. Unzip it and pick the folder as the Vault to go back to it. Sign-ins and API keys are not in the Vault, so they are not in the backup.</p>
    <label class="check"><input type="checkbox" id="s-bk-files" ${keepChecked ? 'checked' : ''}> Also the work files of projects and the shared files (can be large)</label>
    <div class="inline"><button class="btn primary" data-bk="start" ${running ? 'disabled' : ''}>${running ? 'Backing up…' : 'Back up now'}</button>
      ${running ? `<span class="hint" data-no-i18n>${b.files_done} · ${size(b.bytes_done)}</span>` : ''}</div>
    ${b.last?.error ? `<div class="errbox">⚠️ ${esc(b.last.error)}</div>` : ''}
    ${b.last?.skipped ? `<p class="hint"><span data-no-i18n>${b.last.skipped}</span> <span>files could not be read and are not in the backup.</span></p>` : ''}
    <div class="bk-list">${b.backups.map((x) => `<div class="bk-row"><span data-no-i18n>${esc(dateOf(x.name))}</span> <span class="badge">${x.full ? 'Everything' : 'Company data'}</span> <span class="muted small" data-no-i18n>${size(x.size)}</span>
      <span class="grow"></span><a class="btn small" href="/api/backup/download?name=${encodeURIComponent(x.name)}" download>⬇ Download</a>
      <button class="btn small danger" data-bk="del" data-name="${esc(x.name)}">Delete</button></div>`).join('')}</div>`;
}

export async function bindCare(view) {
  const nCard = $('#s-notify', view), bCard = $('#s-backup', view);
  if (!nCard || !bCard) return;
  let timer = 0;
  const paintBackup = (b) => {
    if (!view.contains(bCard)) return;
    const keep = $('#s-bk-files', bCard)?.checked;
    bCard.innerHTML = backupHTML(b, keep);
    clearTimeout(timer);
    if (b.running) timer = setTimeout(guard(async () => paintBackup(await api('GET', '/api/backup'))), 700);
  };
  nCard.innerHTML = notifyHTML(await api('GET', '/api/notifications/settings'));
  paintBackup(await api('GET', '/api/backup'));
  nCard.onchange = guard(async (e) => {
    const k = e.target.dataset.n;
    if (!k) return;
    const r = await api('PUT', '/api/notifications/settings', { [k]: k === 'done_min' ? Math.round(+e.target.value) : e.target.checked });
    if (k === 'done_min') e.target.value = r.done_min;
    toast('Saved');
  });
  bCard.onclick = guard(async (e) => {
    const b = e.target.closest('[data-bk]');
    if (!b) return;
    if (b.dataset.bk === 'start') paintBackup(await api('POST', '/api/backup', { include_files: $('#s-bk-files', bCard).checked }));
    else twoStep(b, 'Really delete?', guard(async () => paintBackup(await api('DELETE', '/api/backup?name=' + encodeURIComponent(b.dataset.name)))));
  });
}
