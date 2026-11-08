import { api } from './api.js';
import { ghostSVG } from './ghosts.js';
import { S, FORMS, PALETTE, skillById, skillScopeBadge, agentById, brainOptions, accountById } from './state.js';
import { t } from './i18n.js';
import { $, esc, guard, modal, closeModal, toast, twoStep } from './ui.js';
import { ghostPickerHTML, bindGhostPicker } from './pickers.js';

const NAMES = ['Boo', 'Casper', 'Wisp', 'Spook', 'Phantom', 'Inky', 'Misty', 'Pip', 'Haze', 'Nimbus', 'Echo', 'Blinky', 'Gus', 'Luna', 'Fable', 'Mallow'];
export const TEMPLATES = [
  { label: 'Planner', role: 'Planner (CEO)', form: 'royal', color: '#ffe27a', skills: ['read_files', 'write_files'], persona: 'You turn goals into clear, small tasks and hand them to the right colleagues. Be concise and decisive.' },
  { label: 'Developer', role: 'Software developer', form: 'robot', color: '#7cc4ff', skills: ['read_files', 'write_files', 'shell_commands'], persona: 'You write clean, working code and test it. Keep changes small and explain what you changed.' },
  { label: 'Writer', role: 'Writer', form: 'cat', color: '#ff8fb8', skills: ['read_files', 'write_files'], persona: 'You write clear, friendly text. Save drafts as Markdown files.' },
  { label: 'Reviewer', role: 'Reviewer', form: 'detective', color: '#b69cff', skills: ['read_files'], persona: 'You review work critically but kindly. You never change files; you report findings with concrete suggestions.' },
  { label: 'Researcher', role: 'Researcher', form: 'scientist', color: '#9ff0d4', skills: ['read_files', 'write_files', 'web_research'], persona: 'You research a question thoroughly and save a short, sourced summary as a Markdown file.' },
  { label: 'Fixer', role: 'Fixer', form: 'ninja', color: '#f2f0ff', skills: ['read_files', 'write_files', 'shell_commands'], persona: 'You quietly find and fix bugs and small problems. Describe each fix in one sentence.' },
];

export function openAgentModal(agent, tpl) {
  const isNew = !agent;
  const st = S.state;
  const firstReal = st.providers.find((p) => p.available && p.id !== 'demo') || st.providers.find((p) => p.id === 'demo');
  const draft = agent ? { ...agent, skills: [...agent.skills] } : {
    name: NAMES[Math.floor(Math.random() * NAMES.length)], role: 'Assistant', provider: firstReal?.id || 'demo', model: '', reports_to: '',
    form: FORMS[Math.floor(Math.random() * FORMS.length)].id, color: PALETTE[Math.floor(Math.random() * PALETTE.length)],
    persona: '', timeout_min: 30, thinking: '', memory: false, enabled: true, skills: ['read_files', 'write_files'].filter((id) => skillById(id)),
  };
  const apply = (t) => Object.assign(draft, { role: t.role, form: t.form, color: t.color, persona: t.persona, skills: t.skills.filter((id) => skillById(id)) });
  if (tpl) apply(tpl);

  const bosses = st.agents.filter((a) => !agent || a.id !== agent.id);
  modal(`
    <h2>${isNew ? 'Hire a ghost' : 'Edit ' + esc(agent.name)}</h2>
    <p class="sub">${isNew ? 'Pick a look, a job, a brain and some skills.' : 'Change anything, any time. Ghosts can shapeshift whenever they like.'}</p>
    ${isNew ? `<div class="chips-row" style="margin:10px 0">${TEMPLATES.map((t, i) => `<button class="tpl" data-tpl="${i}">${esc(t.label)}</button>`).join('')}</div>` : ''}
    <div class="editor">
      <div class="preview" id="prev"></div>
      <div>
        <div class="two"><div><label>Name</label><input type="text" id="f-name" maxlength="40"></div>
          <div><label>Role</label><input type="text" id="f-role" maxlength="60"></div></div>
        <div id="f-pick">${ghostPickerHTML()}</div>
        <div class="two"><div><label>Brain (provider)</label><select id="f-provider">${brainOptions(draft.provider).map((p) => `<option value="${esc(p.id)}" ${p.available || p.id === draft.provider ? '' : 'disabled'}>${esc(p.label)}${p.note}</option>`).join('')}</select></div>
          <div><label>Model (optional)</label><input type="text" id="f-model" placeholder="provider default" list="f-model-list" autocomplete="off"><datalist id="f-model-list"></datalist></div></div>
        <div class="two" id="f-think-row" hidden><div><label>Thinking</label><select id="f-thinking"><option value="">Model default</option><option value="low">Low (faster)</option><option value="medium">Medium</option><option value="high">High (slower, more careful)</option></select></div>
          <div class="hint">For models that think before they answer (e.g. MiniMax, Kimi, GLM). Others ignore it.</div></div>
        <div class="two"><div><label>Reports to</label><select id="f-boss"><option value="">Nobody (top of the company)</option>${bosses.map((b) => `<option value="${esc(b.id)}">${esc(b.name)} — ${esc(b.role)}</option>`).join('')}</select></div>
          <div><label>Time limit per run (minutes)</label><input type="number" id="f-timeout" min="1" max="240"></div></div>
        <label>Skills <span class="hint-inline">what this ghost may do - agents act with the rights of the person asking</span></label>
        <div class="skill-list" id="f-skills">${st.skills.map((s) => `<label class="check" title="${esc(s.description)}"><input type="checkbox" value="${esc(s.id)}"> ${esc(s.name)}${s.min_role === 'owner' ? ' <span class="badge">owner</span>' : ''}${s.enabled ? '' : ' <span class="badge">off</span>'}${skillScopeBadge(s)}</label>`).join('') || '<span class="hint">No skills yet. The owner can add some in Skills.</span>'}</div>
        <label>Personality &amp; instructions</label><textarea id="f-persona" maxlength="8000" placeholder="How should this ghost think, talk and work? Keep it short: it is paid on every run."></textarea>
        <label class="check"><input type="checkbox" id="f-enabled"> Switched on</label>
        <label class="check"><input type="checkbox" id="f-memory"> Long-term memory <span class="hint-inline">keeps short notes between chats, separately for each person</span></label>
        ${isNew ? '' : `<div id="f-mem-box" hidden><h3 class="subhead">What it remembers about you</h3><div id="f-mem-list"></div>
          <div class="inline"><input type="text" id="f-mem-new" maxlength="300" placeholder="Add a note…"><button class="btn small" type="button" id="f-mem-add">Add</button></div></div>`}
      </div></div>
    <div class="modal-foot">
      ${isNew ? '' : '<button class="btn danger" id="f-del" style="margin-right:auto">Fire this ghost</button>'}
      <button class="btn" id="f-cancel">Cancel</button><button class="btn primary" id="f-save">${isNew ? 'Hire' : 'Save'}</button></div>`,
  (m) => {
    const q = (s) => $(s, m);
    const drawPreview = () => {
      q('#prev').style.setProperty('--gc', draft.color);
      q('#prev').innerHTML = ghostSVG({ form: draft.form, color: draft.color, mood: 'idle', size: 150 }) + `<div class="pname">${esc(draft.name || '…')}</div><div class="meta">${esc(draft.role)}</div>`;
    };
    const fill = () => {
      q('#f-name').value = draft.name; q('#f-role').value = draft.role; q('#f-model').value = draft.model; q('#f-timeout').value = draft.timeout_min; q('#f-thinking').value = draft.thinking || '';
      q('#f-provider').value = draft.provider; q('#f-boss').value = draft.reports_to || ''; q('#f-persona').value = draft.persona; q('#f-enabled').checked = draft.enabled; q('#f-memory').checked = !!draft.memory;
      m.querySelectorAll('#f-skills input').forEach((c) => { c.checked = draft.skills.includes(c.value); });
      modelHint();
      drawPreview();
    };
    // Gemini ghosts without a model use gemini-flash-lite-latest (the server's default: Pro has no free API quota); OpenCode
    // ghosts the default model of their key's service (models are written service/model).
    // An OpenCode ghost on an API key account uses that service's default; on Auto it needs a model, e.g. a local one: the
    // list suggests what LM Studio / Ollama offer right now and every service's default.
    let ocModels = null;
    const modelHint = () => {
      const [base, acc] = draft.provider.split('#'), list = q('#f-model-list');
      q('#f-think-row').hidden = base !== 'opencode';
      if (base !== 'opencode') {
        q('#f-model').placeholder = base.split('@')[0] === 'gemini' ? 'gemini-flash-lite-latest' : t('provider default');
        list.innerHTML = '';
        return;
      }
      const svc = (S.state.providers.find((p) => p.id === 'opencode')?.services || []).find((v) => v.id === accountById(acc)?.service);
      q('#f-model').placeholder = svc ? svc.model : t('e.g. lmstudio/<model> or ollama/<model>');
      const fill = () => {
        // Local models, each service's default, then the full list of the account's service (Auto: of every service).
        const all = ocModels?.services || [], seen = new Set();
        list.innerHTML = [...(ocModels?.local || []).map((m) => [m.id, m.server]), ...all.map((v) => [v.model, v.label + ' (API key)']),
          ...all.filter((v) => !svc || v.id === svc.id).flatMap((v) => (v.models || []).map((id) => [id, v.label]))]
          .filter(([id]) => !seen.has(id) && seen.add(id))
          .map(([id, label]) => `<option value="${esc(id)}">${esc(label)}</option>`).join('');
      };
      if (ocModels) fill();
      else api('GET', '/api/opencode/models').then((r) => { ocModels = r; fill(); }).catch(() => {});
    };
    const paintMemory = (notes) => {
      if (!q('#f-mem-list')) return;
      q('#f-mem-list').innerHTML = notes.map((n, i) => `<div class="mem-row"><span data-no-i18n>${esc(n)}</span><button class="btn small" type="button" data-mem-del="${i + 1}" title="Forget">✕</button></div>`).join('') || '<p class="hint">Nothing yet.</p>';
    };
    const showMemory = () => {
      if (!q('#f-mem-box')) return;
      q('#f-mem-box').hidden = !draft.memory;
      if (draft.memory) api('GET', `/api/agents/${agent.id}/memory`).then((r) => paintMemory(r.notes)).catch(() => {});
    };
    showMemory();
    const picker = bindGhostPicker(q('#f-pick'), draft, drawPreview);
    fill();
    m.addEventListener('input', (e) => {
      const map = { 'f-name': 'name', 'f-role': 'role', 'f-model': 'model', 'f-persona': 'persona' };
      if (map[e.target.id]) { draft[map[e.target.id]] = e.target.value; drawPreview(); }
      if (e.target.id === 'f-timeout') draft.timeout_min = +e.target.value;
    });
    m.addEventListener('change', (e) => {
      if (e.target.id === 'f-provider') { draft.provider = e.target.value; modelHint(); }
      if (e.target.id === 'f-boss') draft.reports_to = e.target.value;
      if (e.target.id === 'f-thinking') draft.thinking = e.target.value;
      if (e.target.id === 'f-enabled') draft.enabled = e.target.checked;
      if (e.target.id === 'f-memory') { draft.memory = e.target.checked; showMemory(); }
      if (e.target.closest('#f-skills')) draft.skills = [...m.querySelectorAll('#f-skills input:checked')].map((c) => c.value);
    });
    m.addEventListener('click', guard(async (e) => {
      const forget = e.target.closest('[data-mem-del]');
      if (forget) return paintMemory((await api('DELETE', `/api/agents/${agent.id}/memory/${forget.dataset.memDel}`)).notes);
      if (e.target.id === 'f-mem-add') {
        const box = q('#f-mem-new');
        if (!box.value.trim()) return;
        paintMemory((await api('POST', `/api/agents/${agent.id}/memory`, { text: box.value })).notes);
        box.value = '';
        return;
      }
      const tp = e.target.closest('[data-tpl]');
      if (tp) { apply(TEMPLATES[+tp.dataset.tpl]); fill(); picker.redraw(); return; }
      if (e.target.id === 'f-cancel') return closeModal();
      if (e.target.id === 'f-del') {
        const b = e.target;
        return twoStep(b, 'Really fire?', async () => {
          await api('DELETE', '/api/agents/' + agent.id); closeModal(); toast(agent.name + ' left the haunt 👋'); window.dispatchEvent(new Event('pc2:changed'));
        });
      }
      if (e.target.id === 'f-save') {
        const body = { name: draft.name, role: draft.role, provider: draft.provider, model: draft.model, reports_to: draft.reports_to, form: draft.form, color: draft.color,
          persona: draft.persona, timeout_min: draft.timeout_min, thinking: draft.thinking || '', memory: !!draft.memory, enabled: draft.enabled, skills: draft.skills };
        if (isNew) await api('POST', '/api/agents', body); else await api('PUT', '/api/agents/' + agent.id, body);
        closeModal(); toast(isNew ? draft.name + ' joined the haunt! 👻' : 'Saved'); window.dispatchEvent(new Event('pc2:changed'));
      }
    }));
  }, true);
}
