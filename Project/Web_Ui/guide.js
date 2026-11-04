// The guide ghost: offered right after the company is created, and later from The Haunt while the company has none.
// It has the "Detailed app guide" skill (no tools): it explains the program and helps the person hire a team.
import { lang } from './i18n.js';
import { api } from './api.js';
import { ghostSVG } from './ghosts.js';
import { $, guard, modal, closeModal, toast } from './ui.js';
import { S, refreshState, isOwner } from './state.js';

const GUIDE_HELLO = {
  en: "Hi, I'm your guide! I know every corner of Phantom Co. Tell me what you'd like your ghosts to do (write, code, research, plan...) and I'll help you hire the right team, step by step. Or just ask me how anything works.",
  tr: "Merhaba, ben rehberinim! Phantom Co.'nun her köşesini bilirim. Hayaletlerinin ne yapmasını istediğini söyle (yazı, kod, araştırma, planlama...), doğru ekibi adım adım birlikte kuralım. Ya da herhangi bir şeyin nasıl çalıştığını sor.",
};
const hasBrain = () => (S.state?.providers || []).some((p) => p.available && (p.id === 'claude' || p.id === 'codex' || p.id === 'gemini'));
export const canHireGuide = () => isOwner() && !(S.state?.agents || []).some((a) => (a.skills || []).includes('app_guide'));

export async function hireGuide() {
  const c = await api('POST', '/api/onboarding/guide', { greeting: GUIDE_HELLO[lang] || GUIDE_HELLO.en });
  await refreshState();
  location.hash = '#/chats/' + c.meta.id;
}

export function offerGuide() {
  const brain = hasBrain();
  modal(`<div class="guide-offer">${ghostSVG({ form: 'detective', color: '#9ff0d4', mood: brain ? 'done' : 'idle', size: 110 })}
    <h2>Meet your guide?</h2>
    <p class="sub">${brain ? 'A guide ghost knows the whole program. It answers your questions and helps you hire your first team.'
      : 'A guide ghost can explain the program and help you hire your first team, but it needs Claude Code or Codex on this PC. Install one, sign in, and hire the guide later from The Haunt.'}</p>
    <div class="modal-foot">${brain ? '<button class="btn" id="g-skip">Skip the tour</button><button class="btn primary" id="g-go">Hire the guide</button>'
      : '<button class="btn primary" id="g-skip">Continue</button>'}</div></div>`, (bg) => {
    $('#g-skip', bg).onclick = closeModal;
    const go = $('#g-go', bg);
    if (go) go.onclick = guard(async () => {
      go.disabled = true;
      try { closeModal(); await hireGuide(); } catch (e) { toast(e.message, true); }
    });
  });
}
