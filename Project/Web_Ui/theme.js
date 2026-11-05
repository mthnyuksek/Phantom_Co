// Colour theme of the interface. Per person and per browser/window (localStorage, like the language), chosen in
// the profile. The colours themselves live in app.css ([data-theme=...] blocks); this module only puts the
// chosen id on <html>. "system" follows the Windows light/dark setting and changes with it.

export const THEMES = [
  { id: 'system',   label: 'Same as Windows' },
  { id: 'midnight', label: 'Midnight', ghost: '#b69cff' },
  { id: 'daylight', label: 'Daylight', ghost: '#b69cff' },
  { id: 'swamp',    label: 'Swamp',    ghost: '#9ff0d4' },
  { id: 'deep_sea', label: 'Deep sea', ghost: '#7cc4ff' },
  { id: 'pumpkin',  label: 'Pumpkin',  ghost: '#ffb38a' },
  { id: 'sakura',   label: 'Sakura',   ghost: '#ff8fb8' },
];
const KEY = 'pc2.theme';
const light = matchMedia('(prefers-color-scheme: light)');

function stored() {
  try {
    const v = localStorage.getItem(KEY);
    if (THEMES.some((t) => t.id === v)) return v;
  } catch { /* private mode */ }
  return 'midnight';
}

export let theme = stored();
// The theme actually drawn ("system" resolved).
export const shownTheme = () => (theme === 'system' ? (light.matches ? 'daylight' : 'midnight') : theme);
// Colour of the app's own ghost (sidebar, sign-in screens) in the current theme.
export const brandColor = () => THEMES.find((t) => t.id === shownTheme())?.ghost || '#b69cff';

function apply() {
  const id = shownTheme();
  document.documentElement.dataset.theme = id;
  const bg = getComputedStyle(document.documentElement).getPropertyValue('--bg').trim();
  document.querySelector('meta[name=theme-color]')?.setAttribute('content', bg || '#0c0a14');
  window.dispatchEvent(new CustomEvent('pc2:theme', { detail: id }));
}

export function setTheme(id) {
  if (!THEMES.some((t) => t.id === id)) return;
  theme = id;
  try { localStorage.setItem(KEY, id); } catch { /* private mode */ }
  apply();
}

light.addEventListener('change', () => { if (theme === 'system') apply(); });
apply();

// Picker tiles: each one is drawn in its own theme's colours (app.css .theme-btn).
export const themePickerHTML = () => `<div class="theme-grid">${THEMES.map((t) => `
  <button type="button" class="theme-btn ${t.id === theme ? 'on' : ''}" data-theme-pick="${t.id}">
    <div class="theme-tile ${t.id === 'system' ? 'split' : ''}" data-theme="${t.id === 'system' ? 'midnight' : t.id}" style="background:var(--bg)">
      <i style="background:var(--accent)"></i><i style="background:var(--text)"></i></div>${t.label}</button>`).join('')}</div>`;

export function bindThemePicker(root) {
  root.addEventListener('click', (e) => {
    const b = e.target.closest('[data-theme-pick]');
    if (!b || !root.contains(b)) return;
    setTheme(b.dataset.themePick);
    root.querySelectorAll('[data-theme-pick]').forEach((x) => x.classList.toggle('on', x === b));
  });
}
