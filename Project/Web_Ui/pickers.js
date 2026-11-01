import { ghostSVG } from './ghosts.js';
import { FORMS, PALETTE } from './state.js';
import { $, esc } from './ui.js';

// Form grid + color swatches for a ghost look. `draft` has {form, color}; onChange() runs after every change.
export const ghostPickerHTML = () => `
  <label>Form (shapeshift!)</label><div class="form-grid" data-gp="forms"></div>
  <label>Color</label><div class="swatches" data-gp="colors"></div>`;

export function bindGhostPicker(root, draft, onChange) {
  const forms = $('[data-gp=forms]', root), colors = $('[data-gp=colors]', root);
  const drawForms = () => {
    forms.innerHTML = FORMS.map((f) => `<button type="button" class="form-btn ${f.id === draft.form ? 'on' : ''}" data-form="${f.id}"><div class="tiny">${ghostSVG({ form: f.id, color: draft.color, size: 36 })}</div>${f.label}</button>`).join('');
  };
  const drawColors = () => {
    colors.innerHTML = PALETTE.map((c) => `<span class="sw ${c === draft.color ? 'on' : ''}" style="background:${c}" data-color="${c}"></span>`).join('') +
      `<input type="color" data-gp="custom" value="${esc(draft.color)}" title="Custom color" style="width:32px;height:28px;border:0;background:none;padding:0">`;
  };
  drawForms(); drawColors();
  root.addEventListener('click', (e) => {
    const fb = e.target.closest('[data-form]');
    if (fb && root.contains(fb)) { draft.form = fb.dataset.form; drawForms(); onChange(); return; }
    const sw = e.target.closest('[data-color]');
    if (sw && root.contains(sw)) { draft.color = sw.dataset.color; drawForms(); drawColors(); onChange(); }
  });
  root.addEventListener('input', (e) => {
    if (e.target.dataset?.gp === 'custom') { draft.color = e.target.value; drawForms(); onChange(); }
  });
  return { redraw: () => { drawForms(); drawColors(); } };
}
