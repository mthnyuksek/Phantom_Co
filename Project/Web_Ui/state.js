import { api } from './api.js';
import { ghostSVG, FORMS, PALETTE } from './ghosts.js';

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

export const agentGhost = (a, { mood, size = 110 } = {}) =>
  ghostSVG({ form: a?.form || 'classic', color: a?.color || '#8f89a8', mood: mood || (a?.activity?.state === 'working' ? 'working' : a?.activity?.state === 'queued' ? 'queued' : 'idle'), size });
export const userGhost = (u, size = 34) => ghostSVG({ form: u?.form || 'classic', color: u?.color || '#9ff0d4', mood: 'idle', size });

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

export const providerLabel = (id) => S.state?.providers.find((p) => p.id === id)?.label.split(' ')[0] || id;
