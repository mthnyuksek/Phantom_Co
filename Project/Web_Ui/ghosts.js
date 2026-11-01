// Ghost avatars: one SVG body, many forms (costumes) and moods. Pure functions,
// no dependencies. ghostSVG({form, color, mood}) -> SVG markup string.

export const FORMS = [
  { id: 'classic',   label: 'Classic' },
  { id: 'wizard',    label: 'Wizard' },
  { id: 'pirate',    label: 'Pirate' },
  { id: 'knight',    label: 'Knight' },
  { id: 'robot',     label: 'Robot' },
  { id: 'ninja',     label: 'Ninja' },
  { id: 'chef',      label: 'Chef' },
  { id: 'detective', label: 'Detective' },
  { id: 'royal',     label: 'Royal' },
  { id: 'cat',       label: 'Cat' },
  { id: 'scientist', label: 'Scientist' },
];

export const PALETTE = ['#9ff0d4', '#b69cff', '#ffb38a', '#7cc4ff', '#ff8fb8', '#ffe27a', '#b4f06a', '#f2f0ff', '#ff7a7a', '#5fd6d6'];

const BODY =
  'M18 74 C18 38 36 14 60 14 C84 14 102 38 102 74 L102 114 Q92 132 82 114 Q71 98 60 114 Q49 130 38 114 Q28 98 18 114 Z';
const INK = '#1b1630';

let counter = 0;

function rgb(hex) {
  const n = parseInt(hex.slice(1), 16);
  return [(n >> 16) & 255, (n >> 8) & 255, n & 255];
}
function mix(hex, target, t) {
  const a = rgb(hex), b = target === 'w' ? [255, 255, 255] : [0, 0, 0];
  return '#' + a.map((v, i) => Math.round(v + (b[i] - v) * t).toString(16).padStart(2, '0')).join('');
}

// Each form returns layers drawn at different depths, plus overrides:
//   back  - behind the body      mid - on the body, under the eyes
//   front - above the face       eyes/mouth: 'hide' to replace the default face
const FORM_DEFS = {
  classic: () => ({}),

  wizard: () => ({
    front: `
      <polygon points="34,36 60,-16 86,36" fill="#3b2a7a"/>
      <ellipse cx="60" cy="36" rx="36" ry="8" fill="#4a3796"/>
      <rect x="38" y="26" width="44" height="6" fill="#f5c542" opacity=".9" transform="rotate(-2 60 29)"/>
      <path d="M58 6 l2 -5 l2 5 l5 2 l-5 2 l-2 5 l-2 -5 l-5 -2z" fill="#ffe27a"/>
      <circle cx="50" cy="18" r="1.8" fill="#ffe27a"/><circle cx="70" cy="12" r="1.5" fill="#ffe27a"/>
      <path d="M43 92 Q60 134 77 92 Q60 100 43 92Z" fill="#f3f1ff" stroke="#cfcbe6" stroke-width="1"/>`,
  }),

  pirate: () => ({
    front: `
      <path d="M26 36 Q60 -12 94 36 Q60 24 26 36Z" fill="#1c1c26"/>
      <path d="M24 38 Q60 28 96 38 Q60 46 24 38Z" fill="#2b2b3a"/>
      <circle cx="60" cy="20" r="5" fill="#f3f1ff"/><rect x="57" y="23" width="6" height="3" fill="#f3f1ff"/>
      <circle cx="56.5" cy="19" r="1.3" fill="#1c1c26"/><circle cx="63.5" cy="19" r="1.3" fill="#1c1c26"/>
      <path d="M20 50 L100 76" stroke="#111" stroke-width="2.5"/>
      <ellipse cx="73" cy="66" rx="10" ry="11" fill="#111"/>
      <circle cx="26" cy="88" r="3.2" fill="none" stroke="#f5c542" stroke-width="2"/>`,
  }),

  knight: () => ({
    eyes: 'hide', mouth: 'hide',
    front: `
      <path d="M20 80 C16 36 38 10 60 10 C82 10 104 36 100 80 Z" fill="url(#__id__m)"/>
      <rect x="28" y="56" width="64" height="16" rx="4" fill="#12101c"/>
      <ellipse cx="47" cy="64" rx="6" ry="4.5" fill="#9ff0ff"/><ellipse cx="73" cy="64" rx="6" ry="4.5" fill="#9ff0ff"/>
      <rect x="57" y="18" width="6" height="36" rx="3" fill="#8b94ab"/>
      <path d="M60 12 Q74 -14 98 -2 Q80 4 68 18Z" fill="#d6334f"/>
      <path d="M24 82 Q60 96 96 82" stroke="#8b94ab" stroke-width="3" fill="none"/>`,
    defs: `<linearGradient id="__id__m" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#e3e7f2"/><stop offset="1" stop-color="#8f98b0"/></linearGradient>`,
  }),

  robot: () => ({
    eyes: 'hide', mouth: 'hide',
    mid: `<rect x="12" y="62" width="8" height="16" rx="3" fill="#8793ad"/><rect x="100" y="62" width="8" height="16" rx="3" fill="#8793ad"/>`,
    front: `
      <line x1="60" y1="14" x2="60" y2="-6" stroke="#8793ad" stroke-width="3"/>
      <circle cx="60" cy="-9" r="5" fill="#6ff0ff" class="glow-dot"/>
      <rect x="32" y="52" width="56" height="30" rx="10" fill="#0e1b2a" stroke="#6ff0ff" stroke-width="2"/>
      <rect x="40" y="60" width="14" height="12" rx="3" fill="#6ff0ff"/><rect x="66" y="60" width="14" height="12" rx="3" fill="#6ff0ff"/>
      <path d="M46 92 H74 M46 98 H74" stroke="#2b3a52" stroke-width="3" stroke-linecap="round"/>
      <circle cx="30" cy="40" r="2" fill="#2b3a52"/><circle cx="90" cy="40" r="2" fill="#2b3a52"/>`,
  }),

  ninja: () => ({
    eyes: { fill: '#ffffff', pupil: INK }, mouth: 'hide',
    mid: `<g clip-path="url(#__id__c)">
            <rect x="0" y="52" width="120" height="48" fill="#1f2433"/>
            <rect x="0" y="40" width="120" height="9" fill="#d6334f"/>
          </g>`,
    front: `<path d="M98 42 L116 34 L110 46 L118 54 L100 48Z" fill="#d6334f"/>`,
  }),

  chef: () => ({
    front: `
      <circle cx="42" cy="14" r="15" fill="#fbfbff" stroke="#dcdcec"/><circle cx="60" cy="6" r="17" fill="#fbfbff" stroke="#dcdcec"/>
      <circle cx="78" cy="14" r="15" fill="#fbfbff" stroke="#dcdcec"/>
      <rect x="40" y="16" width="40" height="18" rx="3" fill="#fbfbff" stroke="#dcdcec"/>
      <path d="M47 80 Q54 73 60 78 Q66 73 73 80 Q66 85 60 81 Q54 85 47 80Z" fill="#3a2a2a"/>`,
  }),

  detective: () => ({
    front: `
      <ellipse cx="60" cy="32" rx="42" ry="8" fill="#6b4f2a"/>
      <path d="M38 32 Q38 2 60 2 Q82 2 82 32Z" fill="#7d5d32"/>
      <rect x="38" y="24" width="44" height="6" fill="#4a1f2a"/>
      <circle cx="106" cy="94" r="11" fill="rgba(170,220,255,.28)" stroke="#d9c27a" stroke-width="3"/>
      <line x1="114" y1="102" x2="121" y2="113" stroke="#d9c27a" stroke-width="4" stroke-linecap="round"/>`,
  }),

  royal: () => ({
    back: `<path d="M30 66 L8 134 L112 134 L90 66Z" fill="#a3243b"/><path d="M8 134 L112 134 L110 128 L10 128Z" fill="#f3f1ff" opacity=".85"/>`,
    front: `
      <path d="M34 28 L38 0 L50 14 L60 -6 L70 14 L82 0 L86 28 Z" fill="#f5c542" stroke="#c9961f" stroke-width="1.5"/>
      <circle cx="38" cy="0" r="3" fill="#d6334f"/><circle cx="60" cy="-6" r="3.2" fill="#6ff0ff"/><circle cx="82" cy="0" r="3" fill="#d6334f"/>`,
  }),

  cat: () => ({
    back: `
      <path d="M26 44 L28 8 L54 22Z" fill="__c__"/><path d="M94 44 L92 8 L66 22Z" fill="__c__"/>
      <path d="M32 36 L33 17 L46 25Z" fill="#ff9fc0"/><path d="M88 36 L87 17 L74 25Z" fill="#ff9fc0"/>
      <path d="M100 112 Q122 106 116 82 Q114 98 98 100Z" fill="__c__"/>`,
    front: `
      <path d="M57 79 L63 79 L60 83Z" fill="#ff7aa8"/>
      <path d="M26 84 L42 86 M26 92 L42 90 M94 84 L78 86 M94 92 L78 90" stroke="${INK}" stroke-width="1.6" stroke-linecap="round" opacity=".6"/>`,
  }),

  scientist: () => ({
    front: `
      <path d="M34 32 L36 12 L46 24 L52 6 L60 22 L68 6 L74 24 L84 12 L86 32 Q60 20 34 32Z" fill="#e4e7f2"/>
      <circle cx="47" cy="66" r="12" fill="rgba(255,255,255,.14)" stroke="#cfd6e6" stroke-width="2.5"/>
      <circle cx="73" cy="66" r="12" fill="rgba(255,255,255,.14)" stroke="#cfd6e6" stroke-width="2.5"/>
      <path d="M59 65 H61" stroke="#cfd6e6" stroke-width="2.5"/>
      <path d="M104 98 l5 -10 h6 l5 10 v4 h-16z" fill="rgba(120,255,200,.55)" stroke="#cfd6e6" stroke-width="1.5"/>`,
  }),
};

function eyesFor(mood, style) {
  const fill = style && style.fill ? style.fill : INK;
  const pupil = style && style.pupil;
  const L = 47, R = 73, Y = 66;
  if (mood === 'queued')
    return `<path d="M41 ${Y + 2} Q${L} ${Y + 7} 53 ${Y + 2} M67 ${Y + 2} Q${R} ${Y + 7} 79 ${Y + 2}" stroke="${INK}" stroke-width="3" fill="none" stroke-linecap="round"/>`;
  if (mood === 'done')
    return `<path d="M41 ${Y + 3} Q${L} ${Y - 6} 53 ${Y + 3} M67 ${Y + 3} Q${R} ${Y - 6} 79 ${Y + 3}" stroke="${INK}" stroke-width="3.2" fill="none" stroke-linecap="round"/>`;
  if (mood === 'failed') {
    const x = (cx) => `M${cx - 5} ${Y - 5} L${cx + 5} ${Y + 5} M${cx + 5} ${Y - 5} L${cx - 5} ${Y + 5}`;
    return `<path d="${x(L)} ${x(R)}" stroke="${INK}" stroke-width="3" stroke-linecap="round"/>`;
  }
  const ry = mood === 'working' ? 10.5 : 9;
  let s = `<g class="eyes"><ellipse cx="${L}" cy="${Y}" rx="6.5" ry="${ry}" fill="${fill}"/><ellipse cx="${R}" cy="${Y}" rx="6.5" ry="${ry}" fill="${fill}"/>`;
  if (pupil) s += `<circle cx="${L}" cy="${Y + 1}" r="3.2" fill="${pupil}"/><circle cx="${R}" cy="${Y + 1}" r="3.2" fill="${pupil}"/>`;
  else s += `<circle cx="${L + 2}" cy="${Y - 3}" r="2.2" fill="#fff"/><circle cx="${R + 2}" cy="${Y - 3}" r="2.2" fill="#fff"/>`;
  return s + '</g>';
}

function mouthFor(mood) {
  if (mood === 'working' || mood === 'done') return `<path d="M52 85 Q60 94 68 85" stroke="${INK}" stroke-width="3" fill="none" stroke-linecap="round"/>`;
  if (mood === 'failed') return `<path d="M52 91 Q56 85 60 90 Q64 95 68 89" stroke="${INK}" stroke-width="3" fill="none" stroke-linecap="round"/>`;
  if (mood === 'queued') return `<ellipse cx="60" cy="88" rx="3" ry="2.4" fill="${INK}" opacity=".8"/>`;
  return `<ellipse cx="60" cy="87" rx="4" ry="5" fill="${INK}"/>`;
}

function moodExtras(mood) {
  if (mood === 'working')
    return `<g class="sparkle"><path d="M104 24 l2.5 -7 l2.5 7 l7 2.5 l-7 2.5 l-2.5 7 l-2.5 -7 l-7 -2.5z" fill="#ffe27a"/></g>
            <g class="sparkle s2"><path d="M12 40 l1.8 -5 l1.8 5 l5 1.8 l-5 1.8 l-1.8 5 l-1.8 -5 l-5 -1.8z" fill="#9ff0d4"/></g>`;
  if (mood === 'queued')
    return `<text x="98" y="22" font-size="16" font-weight="700" fill="#b8b2d6" class="zzz">z</text><text x="108" y="10" font-size="11" font-weight="700" fill="#b8b2d6" class="zzz z2">z</text>`;
  if (mood === 'failed') return `<g fill="#ff7a7a"><circle cx="100" cy="20" r="2.4"/><circle cx="108" cy="30" r="1.8"/></g>`;
  return '';
}

export function ghostSVG({ form = 'classic', color = '#b69cff', mood = 'idle', size = 120 } = {}) {
  const id = 'gh' + ++counter;
  const def = (FORM_DEFS[form] || FORM_DEFS.classic)();
  const sub = (s) => (s || '').replaceAll('__id__', id).replaceAll('__c__', color);
  const top = mix(color, 'w', 0.42), bot = mix(color, 'k', 0.18), arm = mix(color, 'k', 0.1);
  const eyes = def.eyes === 'hide' ? '' : eyesFor(mood, def.eyes);
  const mouth = def.mouth === 'hide' ? '' : mouthFor(mood);
  const blush = def.mouth === 'hide' ? '' : `<ellipse cx="35" cy="80" rx="6" ry="3.6" fill="#ff7aa8" opacity=".32"/><ellipse cx="85" cy="80" rx="6" ry="3.6" fill="#ff7aa8" opacity=".32"/>`;
  return `<svg class="ghost g-${mood}" viewBox="0 -22 120 162" width="${size}" height="${Math.round(size * 1.35)}" style="--gc:${color}" role="img" aria-label="${form} ghost">
  <defs>
    <linearGradient id="${id}b" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="${top}"/><stop offset=".55" stop-color="${color}"/><stop offset="1" stop-color="${bot}"/></linearGradient>
    <clipPath id="${id}c"><path d="${BODY}"/></clipPath>
    ${sub(def.defs)}
  </defs>
  <g class="body-wrap">
    ${sub(def.back)}
    <ellipse cx="13" cy="86" rx="10" ry="6.5" fill="${arm}" transform="rotate(35 13 86)"/>
    <ellipse cx="107" cy="86" rx="10" ry="6.5" fill="${arm}" transform="rotate(-35 107 86)"/>
    <path d="${BODY}" fill="url(#${id}b)"/>
    <ellipse cx="44" cy="40" rx="14" ry="8" fill="#fff" opacity=".22" transform="rotate(-25 44 40)"/>
    ${sub(def.mid)}
    ${blush}${eyes}${mouth}
    ${sub(def.front)}
    ${moodExtras(mood)}
  </g>
</svg>`;
}
