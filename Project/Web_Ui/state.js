import { api } from './api.js';
import { ghostSVG, FORMS, PALETTE } from './ghosts.js';
import { esc } from './ui.js';

export { FORMS, PALETTE };
export const S = { state: null };
let last = '';

// Fetches the shared snapshot (agents, skills, projects, users...). Returns true when it changed.
export async function refreshState() {
  const s = await api('GET', '/api/state');
  const j = JSON.stringify(s);
  const changed = j !== last;
  last = j;
  S.state = s;
  return changed;
}
export const forgetState = () => { last = ''; S.state = null; };

export const me = () => S.state?.me;
export const isOwner = () => me()?.role === 'owner';
export const canEdit = () => me()?.role === 'owner' || me()?.role === 'editor';
export const agentById = (id) => S.state?.agents.find((a) => a.id === id);
export const userById = (id) => S.state?.users.find((u) => u.id === id);
export const projectById = (id) => S.state?.projects.find((p) => p.id === id);
export const skillById = (id) => S.state?.skills.find((s) => s.id === id);
// Badge for a skill that is used only in some projects ('' when it is used everywhere).
export const skillScopeBadge = (s) => s?.projects?.length
  ? ` <span class="badge" title="Only in these projects">📁 ${esc(s.projects.map((id) => projectById(id)?.name || id).join(', '))}</span>` : '';

export const agentGhost = (a, { mood, size = 110 } = {}) =>
  ghostSVG({ form: a?.form || 'classic', color: a?.color || '#8f89a8', mood: mood || (a?.activity?.state === 'working' ? 'working' : a?.activity?.state === 'queued' ? 'queued' : 'idle'), size });
// a person sleeps while none of their windows is open (u.online from the server)
export const userGhost = (u, size = 34) => ghostSVG({ form: u?.form || 'classic', color: u?.color || '#9ff0d4', mood: u && u.online === false ? 'asleep' : 'idle', size });

// a person's or agent's small avatar by sender string ("user:id" / "agent:id")
export function senderInfo(from) {
  if (from.startsWith('agent:')) {
    const a = agentById(from.slice(6));
    return { kind: 'agent', name: a?.name || from.slice(6), svg: agentGhost(a, { mood: 'idle', size: 34 }), color: a?.color };
  }
  if (from.startsWith('user:')) {
    const u = userById(from.slice(5));
    return { kind: 'user', id: from.slice(5), name: u?.name || 'Someone', svg: userGhost(u), color: u?.color };
  }
  return { kind: 'system', name: '' };
}

// Brains: "claude" (Auto: the CLI's own sign-in), "claude#<account id>" (another sign-in, Settings > Secrets),
// "claude@<user id>" (a friend's PC lends its CLI).
export const accountById = (id) => S.state?.accounts?.find((a) => a.id === id);
export const providerLabel = (id) => {
  const [rest, who] = String(id).split('@');
  const [base, acc] = rest.split('#');
  const label = S.state?.providers.find((p) => p.id === base)?.label.split(' ')[0] || base;
  if (acc) return `${label} · ${accountById(acc)?.name || '?'}`;
  if (!who) return label;
  const w = S.state?.workers?.find((x) => x.user_id === who);
  return `${label} @ ${w?.name || userById(who)?.name || who}`;
};

// Every brain an agent can use: this PC's CLIs (Auto + extra accounts) and the CLIs of friends who currently lend theirs.
// Without extra accounts the list looks exactly as before ("Claude Code", "Codex", "Demo").
export function brainOptions(current) {
  const out = [];
  const accounts = S.state?.accounts || [];
  for (const p of S.state?.providers || []) {
    const mine = accounts.filter((a) => a.provider === p.id);
    const note = p.available ? '' : ' — not installed on this PC';
    out.push({ id: p.id, label: mine.length ? `${p.label} · Auto (this PC's sign-in)` : p.label, available: p.available, note });
    for (const a of mine) out.push({ id: `${p.id}#${a.id}`, label: `${p.label} · ${a.name}${a.kind === 'api_key' ? ' (API)' : ''}`, available: p.available, note });
  }
  for (const w of S.state?.workers || [])
    for (const id of ['claude', 'codex']) {
      const on = w.online && w.providers.includes(id);
      const label = (id === 'claude' ? 'Claude Code' : 'Codex') + ` on ${w.name}'s PC (experimental)`;
      out.push({ id: `${id}@${w.user_id}`, label, available: on, note: on ? '' : ' — offline right now' });
    }
  if (current && !out.some((o) => o.id === current)) out.push({ id: current, label: providerLabel(current), available: true, note: '' });
  return out;
}
