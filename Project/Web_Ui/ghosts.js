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
  { id: 'vampire',   label: 'Vampire' },
  { id: 'witch',     label: 'Witch' },
  { id: 'astronaut', label: 'Astronaut' },
  { id: 'viking',    label: 'Viking' },
  { id: 'cowboy',    label: 'Cowboy' },
  { id: 'artist',    label: 'Artist' },
  { id: 'dj',        label: 'DJ' },
  { id: 'angel',     label: 'Angel' },
  { id: 'devil',     label: 'Imp' },
  { id: 'graduate',  label: 'Graduate' },
  { id: 'mummy',     label: 'Mummy' },
  { id: 'sprout',    label: 'Sprout' },
];

export const PALETTE = ['#9ff0d4', '#b69cff', '#ffb38a', '#7cc4ff', '#ff8fb8', '#ffe27a', '#b4f06a', '#f2f0ff', '#ff7a7a', '#5fd6d6', '#c3c9d9', '#f5c542'];

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

  vampire: () => ({
    mouth: 'hide',
    back: `
      <path d="M24 58 Q8 100 4 134 L116 134 Q112 100 96 58Z" fill="#1c1020"/>
      <path d="M30 70 Q18 104 16 128 L104 128 Q102 104 90 70Z" fill="#7a1430"/>
      <path d="M22 62 L4 22 L42 44Z M98 62 L116 22 L78 44Z" fill="#2a1626"/>`,
    front: `
      <path d="M22 50 C22 20 42 12 60 12 C78 12 98 20 98 50 Q80 34 60 46 Q40 34 22 50Z" fill="#1a1424"/>
      <path d="M51 84 Q60 92 69 84" stroke="${INK}" stroke-width="3" fill="none" stroke-linecap="round"/>
      <path d="M53.5 86.5 l2.4 7 l2.4 -6 Z M62 87.5 l2.4 6 l2.4 -7 Z" fill="#fff"/>`,
  }),

  witch: () => ({
    front: `
      <ellipse cx="60" cy="36" rx="46" ry="8" fill="#231a33"/>
      <path d="M36 36 L56 -12 Q64 -20 74 -14 Q80 -10 86 -14 Q82 -4 72 -6 L84 36Z" fill="#2e2244"/>
      <path d="M38 28 L82 28 L84 36 L36 36Z" fill="#8a3fd1"/>
      <rect x="55" y="27" width="10" height="9" rx="1.5" fill="none" stroke="#f5c542" stroke-width="2"/>
      <path d="M8 118 L26 84" stroke="#8b5a2b" stroke-width="3.5" stroke-linecap="round"/>
      <path d="M2 116 Q0 128 6 132 Q12 126 16 122 Q10 118 2 116Z" fill="#d9a84a"/>`,
  }),

  astronaut: () => ({
    front: `
      <circle cx="60" cy="64" r="50" fill="rgba(170,220,255,.14)" stroke="#e8edf7" stroke-width="4"/>
      <path d="M26 44 Q34 24 54 18" stroke="#fff" stroke-width="4" opacity=".55" fill="none" stroke-linecap="round"/>
      <path d="M14 106 Q60 124 106 106" stroke="#e8edf7" stroke-width="7" fill="none" stroke-linecap="round"/>
      <rect x="54" y="106" width="12" height="9" rx="2" fill="#d6334f"/><circle cx="60" cy="110.5" r="2" fill="#fff"/>
      <line x1="94" y1="30" x2="104" y2="12" stroke="#e8edf7" stroke-width="2.5"/>
      <circle cx="104" cy="10" r="3.5" fill="#ff7a7a" class="glow-dot"/>`,
  }),

  viking: () => ({
    front: `
      <path d="M30 38 Q22 26 16 4 Q28 18 40 26Z" fill="#f3ecd6" stroke="#cfc6a8" stroke-width="1"/>
      <path d="M90 38 Q98 26 104 4 Q92 18 80 26Z" fill="#f3ecd6" stroke="#cfc6a8" stroke-width="1"/>
      <path d="M28 40 Q28 8 60 8 Q92 8 92 40Z" fill="#9aa3b8"/>
      <path d="M57 9 h6 v31 h-6z" fill="#7d869c"/>
      <rect x="25" y="36" width="70" height="8" rx="3" fill="#6b4f2a"/>
      <circle cx="36" cy="40" r="1.8" fill="#d9c27a"/><circle cx="60" cy="40" r="1.8" fill="#d9c27a"/><circle cx="84" cy="40" r="1.8" fill="#d9c27a"/>
      <path d="M32 82 Q36 120 60 126 Q84 120 88 82 Q76 98 60 96 Q44 98 32 82Z" fill="#e07b2c"/>
      <path d="M46 104 v12 M60 108 v14 M74 104 v12" stroke="#b85d1c" stroke-width="2" stroke-linecap="round"/>
      <path d="M46 82 Q53 77 60 82 Q67 77 74 82 Q67 87 60 84 Q53 87 46 82Z" fill="#c9661f"/>`,
  }),

  cowboy: () => ({
    mid: `<g clip-path="url(#__id__c)">
            <path d="M0 96 Q60 112 120 96 L120 104 Q60 120 0 104Z" fill="#d6334f"/>
            <path d="M48 108 L60 126 L72 108Z" fill="#b8243e"/>
            <circle cx="40" cy="104" r="1.6" fill="#fff"/><circle cx="80" cy="104" r="1.6" fill="#fff"/>
          </g>`,
    front: `
      <path d="M34 34 Q30 2 46 6 Q60 14 74 6 Q90 2 86 34Z" fill="#a06a33"/>
      <path d="M53 8 Q60 16 67 8" stroke="#7d5126" stroke-width="2" fill="none"/>
      <rect x="34" y="26" width="52" height="6" fill="#4a2a14"/>
      <path d="M10 32 Q18 46 60 42 Q102 46 110 32 Q104 28 94 34 Q60 40 26 34 Q16 28 10 32Z" fill="#8a5a2b"/>
      <path d="M92 104 l2.2 4.6 l5 .6 l-3.7 3.4 l1 5 l-4.5 -2.5 l-4.5 2.5 l1 -5 l-3.7 -3.4 l5 -.6z" fill="#f5c542" stroke="#c9961f" stroke-width=".8"/>`,
  }),

  artist: () => ({
    front: `
      <ellipse cx="54" cy="22" rx="32" ry="11" fill="#d6334f" transform="rotate(-12 54 22)"/>
      <rect x="44" y="4" width="4" height="8" rx="2" fill="#a3243b" transform="rotate(-12 46 8)"/>
      <path d="M96 98 Q98 80 114 82 Q126 88 120 102 Q114 110 104 106 Q110 98 102 98 Q98 102 96 98Z" fill="#e8c99a" stroke="#c9a36a" stroke-width="1"/>
      <circle cx="108" cy="88" r="2.6" fill="#7cc4ff"/><circle cx="116" cy="92" r="2.6" fill="#ff7a7a"/><circle cx="114" cy="100" r="2.6" fill="#ffe27a"/>
      <line x1="6" y1="104" x2="22" y2="72" stroke="#6b4f2a" stroke-width="3" stroke-linecap="round"/>
      <path d="M22 72 l2 -7 l3 2 z" fill="#b4f06a"/>
      <circle cx="80" cy="108" r="3" fill="#7cc4ff" opacity=".8"/><circle cx="86" cy="113" r="1.8" fill="#ff7a7a" opacity=".8"/>`,
  }),

  dj: () => ({
    front: `
      <path d="M20 66 Q16 8 60 8 Q104 8 100 66" stroke="#2b2b3a" stroke-width="6" fill="none"/>
      <rect x="10" y="54" width="15" height="26" rx="7" fill="#d6334f"/><rect x="95" y="54" width="15" height="26" rx="7" fill="#d6334f"/>
      <rect x="13" y="58" width="5" height="18" rx="2.5" fill="#ff7a96"/><rect x="102" y="58" width="5" height="18" rx="2.5" fill="#ff7a96"/>
      <g fill="#ffe27a"><ellipse cx="13" cy="18" rx="4.2" ry="3.2"/><rect x="15.6" y="2" width="1.8" height="16"/><path d="M15.6 2 q6 2 7 8 q-3 -4 -7 -4z"/></g>`,
  }),

  angel: () => ({
    back: `
      <path d="M22 76 C0 70 -6 46 4 30 C8 40 14 44 22 46 C12 50 14 58 24 60 C16 64 18 70 24 70Z" fill="#fbfbff" stroke="#dcdcec" stroke-width="1"/>
      <path d="M98 76 C120 70 126 46 116 30 C112 40 106 44 98 46 C108 50 106 58 96 60 C104 64 102 70 96 70Z" fill="#fbfbff" stroke="#dcdcec" stroke-width="1"/>`,
    front: `<ellipse cx="60" cy="0" rx="22" ry="6" fill="none" stroke="#ffe27a" stroke-width="4"/>
            <ellipse cx="60" cy="0" rx="22" ry="6" fill="none" stroke="#fff6c2" stroke-width="1.2" opacity=".8"/>`,
  }),

  devil: () => ({
    back: `
      <path d="M96 110 Q122 116 116 90" stroke="#c22a45" stroke-width="3.5" fill="none" stroke-linecap="round"/>
      <path d="M116 80 L122 94 L108 91Z" fill="#c22a45"/>`,
    front: `
      <path d="M32 32 Q20 12 28 -2 Q34 16 46 22Z" fill="#c22a45"/>
      <path d="M88 32 Q100 12 92 -2 Q86 16 74 22Z" fill="#c22a45"/>
      <path d="M39 53 L54 58 M81 53 L66 58" stroke="${INK}" stroke-width="2.6" stroke-linecap="round"/>`,
  }),

  graduate: () => ({
    front: `
      <path d="M36 22 L36 36 Q60 44 84 36 L84 22 L60 30Z" fill="#2b3145"/>
      <path d="M60 4 L106 18 L60 32 L14 18Z" fill="#1f2433"/>
      <circle cx="60" cy="18" r="2.2" fill="#f5c542"/>
      <path d="M60 18 L96 26 L96 42" stroke="#f5c542" stroke-width="2" fill="none"/>
      <path d="M93 42 h6 l1 8 h-8z" fill="#f5c542"/>
      <g transform="rotate(-25 13 96)"><rect x="0" y="91" width="26" height="9" rx="4.5" fill="#f3ecd6" stroke="#cfc6a8"/>
        <rect x="11" y="90" width="4" height="11" fill="#d6334f"/></g>`,
  }),

  mummy: () => ({
    mid: `<g clip-path="url(#__id__c)" stroke="#efe9d8" stroke-width="7" opacity=".93" fill="none">
            <path d="M0 34 L120 48 M0 50 L120 38 M0 96 L120 110 M0 114 L120 96 M0 84 L36 86 M84 86 L120 82 M0 128 L120 124"/>
          </g>
          <g clip-path="url(#__id__c)" stroke="#cfc6a8" stroke-width="1" fill="none" opacity=".8">
            <path d="M0 37.5 L120 51.5 M0 99.5 L120 113.5"/>
          </g>`,
    front: `<path d="M98 108 Q110 116 104 128" stroke="#efe9d8" stroke-width="6" fill="none" stroke-linecap="round"/>`,
  }),

  sprout: () => ({
    front: `
      <path d="M60 16 Q57 6 60 -4" stroke="#4c9a3a" stroke-width="3" fill="none" stroke-linecap="round"/>
      <path d="M59 0 Q44 -14 34 -4 Q46 6 59 0Z" fill="#6fd14f"/>
      <path d="M61 -4 Q76 -20 88 -8 Q74 4 61 -4Z" fill="#8ee46a"/>
      <path d="M59 0 Q48 -4 40 -4 M61 -4 Q72 -10 82 -9" stroke="#4c9a3a" stroke-width="1.2" fill="none"/>
      <circle cx="102" cy="104" r="5" fill="#ffe27a"/>
      <g fill="#ff8fb8"><circle cx="102" cy="97" r="4"/><circle cx="109" cy="104" r="4"/><circle cx="102" cy="111" r="4"/><circle cx="95" cy="104" r="4"/></g>
      <circle cx="102" cy="104" r="3.4" fill="#ffe27a"/>`,
  }),
};

function eyesFor(mood, style) {
  const fill = style && style.fill ? style.fill : INK;
  const pupil = style && style.pupil;
  const L = 47, R = 73, Y = 66;
  if (mood === 'queued' || mood === 'asleep')
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
  if (mood === 'queued' || mood === 'asleep') return `<ellipse cx="60" cy="88" rx="3" ry="2.4" fill="${INK}" opacity=".8"/>`;
  return `<ellipse cx="60" cy="87" rx="4" ry="5" fill="${INK}"/>`;
}

function moodExtras(mood) {
  if (mood === 'working')
    return `<g class="sparkle"><path d="M104 24 l2.5 -7 l2.5 7 l7 2.5 l-7 2.5 l-2.5 7 l-2.5 -7 l-7 -2.5z" fill="#ffe27a"/></g>
            <g class="sparkle s2"><path d="M12 40 l1.8 -5 l1.8 5 l5 1.8 l-5 1.8 l-1.8 5 l-1.8 -5 l-5 -1.8z" fill="#9ff0d4"/></g>`;
  if (mood === 'queued' || mood === 'asleep')
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
