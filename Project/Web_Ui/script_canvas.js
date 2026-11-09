// The canvas of a visual script: blocks joined by arrows, drawn top to bottom (Development/Docs/visual_scripts.md).
// Pan by dragging empty space (or two fingers), zoom with the wheel / a pinch. Blocks come from the right-click menu or
// the palette (＋, slides in from the left). Connect by dragging from an output to an input, or tap an output, then an input.
// The page owns the script object: the canvas changes it in place and calls onChange().
import { esc } from './ui.js';

export const BLOCKS = {
  start: { icon: '▶', label: 'Start', hint: 'Where the run begins', outs: ['out'], hasIn: false },
  prompt: { icon: '💬', label: 'Prompt', hint: 'A ghost does a job', outs: ['out', 'failed'], hasIn: true },
  ask: { icon: '❓', label: 'Ask', hint: 'Wait for a person: approve, reject or give a value', outs: ['approved', 'rejected'], hasIn: true },
  set: { icon: '🧮', label: 'Set variable', hint: 'Give a variable a new value', outs: ['out'], hasIn: true },
  if: { icon: '🔀', label: 'If', hint: 'Go on by a yes / no question', outs: ['true', 'false'], hasIn: true },
  delay: { icon: '⏳', label: 'Delay', hint: 'Wait some minutes or until a time', outs: ['out'], hasIn: true },
  do: { icon: '🔁', label: 'Do N', hint: 'Repeat the loop part N times, then go on', outs: ['loop', 'then'], hasIn: true },
  end: { icon: '⏹', label: 'End', hint: 'The run stops here', outs: [], hasIn: true },
  reroute: { icon: '•', label: 'Reroute', hint: 'A small point that leads an arrow around other blocks. It does nothing.', outs: ['out'], hasIn: true, tiny: true },
};
const PALETTE = ['prompt', 'ask', 'set', 'if', 'delay', 'do', 'reroute', 'end', 'start'];
const NODE_W = 210, TINY = 24, GRID = 20, MIN_ZOOM = 0.25, MAX_ZOOM = 2;
const PORT_LABEL = { true: 'yes', false: 'no', loop: 'loop', then: 'then', failed: 'failed', approved: 'approved', rejected: 'rejected' };
const PORT_MENU = { ...PORT_LABEL, out: 'when it is done' };

const snap = (v) => Math.round(v / GRID) * GRID;
const widthOf = (type) => (BLOCKS[type]?.tiny ? TINY : NODE_W);
const outX = (type, port) => {
  const outs = BLOCKS[type]?.outs || [], w = widthOf(type);
  return outs.length < 2 ? w / 2 : w * (outs.indexOf(port) === 0 ? 0.3 : 0.7);
};

export function createCanvas(host, opts) {
  const o = { readOnly: false, summary: () => '', onChange: () => {}, onSelect: () => {}, onWarn: () => {}, ...opts };
  let script = o.script;
  let sel = null;            // {kind: 'node', id} | {kind: 'edge', index}
  let errors = new Map();    // node id -> [messages]
  let states = new Map();    // node id -> run state (run view)
  let edgeStates = [];       // per arrow: pending | fired | skipped (run view)
  let pending = null;        // tap-to-connect: {from, port} from an output, {to} from an input
  let pan = { x: 0, y: 0 }, zoom = 1;
  const heights = new Map();

  host.innerHTML = `<div class="sc-canvas ${o.readOnly ? 'ro' : ''}" tabindex="0">
      <div class="sc-world"><svg class="sc-edges" width="1" height="1"></svg><div class="sc-nodes"></div></div>
      <div class="sc-tools">
        ${o.readOnly ? '' : '<button class="btn small" data-sc="palette" title="Add a block">＋ Block</button>'}
        <button class="btn small" data-sc="zoomin" title="Zoom in">+</button><button class="btn small" data-sc="zoomout" title="Zoom out">−</button>
        <button class="btn small" data-sc="fit" title="Show everything">⤢</button>
        ${o.readOnly ? '' : '<button class="btn small" data-sc="tidy" title="Arrange the blocks top to bottom">Tidy</button>'}
      </div>
      ${o.readOnly ? '' : `<div class="sc-palette"><div class="sc-pal-head">Blocks <button class="btn small" data-sc="palette" title="Close">✕</button></div>
        ${PALETTE.map((t) => `<div class="sc-pal-item" data-add="${t}"><i>${BLOCKS[t].icon}</i><div><b data-no-i18n>${BLOCKS[t].label}</b><small>${BLOCKS[t].hint}</small></div></div>`).join('')}
        <p class="sc-pal-tip">Drag a block onto the canvas, or tap it.</p></div>`}
      <div class="sc-menu" hidden></div>
      <div class="sc-banner" hidden><span>Now tap the block this should lead to.</span> <button class="btn small" data-sc="cancel-link">Cancel</button></div>
    </div>`;
  const root = host.firstElementChild;
  const world = root.querySelector('.sc-world');
  const svg = root.querySelector('.sc-edges');
  const nodesEl = root.querySelector('.sc-nodes');
  const menu = root.querySelector('.sc-menu');

  // ------------------------------------------------------------------ geometry
  const nodeById = (id) => script.nodes.find((n) => n.id === id);
  const applyView = () => { world.style.transform = `translate(${pan.x}px, ${pan.y}px) scale(${zoom})`; };
  const toWorld = (cx, cy) => {
    const r = root.getBoundingClientRect();
    return { x: (cx - r.left - pan.x) / zoom, y: (cy - r.top - pan.y) / zoom };
  };
  const isTiny = (n) => !!BLOCKS[n.type]?.tiny;
  // A reroute point is one spot: arrows end in its middle and may come from any side.
  const portIn = (n) => (isTiny(n) ? { x: n.x + TINY / 2, y: n.y + TINY / 2 } : { x: n.x + NODE_W / 2, y: n.y });
  const portOut = (n, port) => (isTiny(n) ? portIn(n) : { x: n.x + outX(n.type, port), y: n.y + (heights.get(n.id) || 64) });
  const curve = (a, b, freeA, freeB) => {  // a free end has no vertical start
    const dy = Math.max(40, Math.abs(b.y - a.y) / 2);
    return `M${a.x},${a.y} C${a.x},${freeA ? a.y : a.y + dy} ${b.x},${freeB ? b.y : b.y - dy} ${b.x},${b.y}`;
  };

  // ------------------------------------------------------------------ drawing
  function nodeHTML(n) {
    const b = BLOCKS[n.type] || { icon: '?', label: n.type, outs: [], hasIn: true };
    const errs = errors.get(n.id);
    const cls = ['sc-node', 'k-' + n.type, sel?.kind === 'node' && sel.id === n.id ? 'sel' : '', errs ? 'err' : '', states.has(n.id) ? 'st-' + states.get(n.id) : ''].join(' ');
    const outCls = (p) => (pending?.to ? 'target' : pending && pending.from === n.id && pending.port === p ? 'armed' : '');
    const inCls = pending?.to === n.id ? 'armed' : pending?.from ? 'target' : '';
    const OUT_TIP = 'Drag to another block, or to an empty place to add one', IN_TIP = 'Drag to the block this should come from, or to an empty place to add one';
    if (b.tiny) return `<div class="${cls} sc-tiny" data-node="${esc(n.id)}" style="left:${n.x}px;top:${n.y}px;width:${TINY}px;height:${TINY}px" ${errs ? `title="${esc(errs[0])}"` : ''}>
      <span class="sc-port in ${inCls}" data-in="1" title="${IN_TIP}"></span><span class="sc-port out ${outCls('out')}" data-out="out" title="${OUT_TIP}"></span></div>`;
    const outs = b.outs.map((p) => `<span class="sc-port out ${outCls(p)}" data-out="${p}" style="left:${outX(n.type, p)}px" title="${OUT_TIP}">${PORT_LABEL[p] ? `<em>${PORT_LABEL[p]}</em>` : ''}</span>`).join('');
    return `<div class="${cls}" data-node="${esc(n.id)}" style="left:${n.x}px;top:${n.y}px;width:${NODE_W}px" ${errs ? `title="${esc(errs[0] + (errs.length > 1 ? ` (+${errs.length - 1})` : ''))}"` : ''}>
      ${b.hasIn ? `<span class="sc-port in ${inCls}" data-in="1" title="${IN_TIP}"></span>` : ''}
      <div class="sc-node-head"><i>${b.icon}</i><b data-no-i18n>${esc(n.label || b.label)}</b>${errs ? '<span class="sc-err-dot">!</span>' : ''}</div>
      <div class="sc-node-sum">${o.summary(n)}</div>${outs}</div>`;
  }
  function drawEdges() {
    let html = '';
    script.edges.forEach((e, i) => {
      const a = nodeById(e.from), b = nodeById(e.to);
      if (!a || !b) return;
      const d = curve(portOut(a, e.port || 'out'), portIn(b), isTiny(a), isTiny(b));
      const on = sel?.kind === 'edge' && sel.index === i;
      const cls = e.port === 'true' || e.port === 'approved' ? 'yes' : e.port === 'false' || e.port === 'rejected' || e.port === 'failed' ? 'no' : e.port === 'loop' ? 'loop' : '';
      const st = edgeStates[i] === 'fired' ? 'lit' : edgeStates[i] === 'skipped' ? 'dim' : '';
      html += `<path class="sc-edge ${cls} ${on ? 'sel' : ''} ${st}" d="${d}"/><path class="sc-hit" data-edge="${i}" d="${d}"/>`;
    });
    html += '<path class="sc-temp" d="" />';
    svg.innerHTML = html;
  }
  function render() {
    root.querySelector('.sc-banner').hidden = !pending;
    root.querySelector('.sc-banner span').textContent = pending?.to ? 'Now tap the block this should come from.' : 'Now tap the block this should lead to.';
    nodesEl.innerHTML = script.nodes.map(nodeHTML).join('');
    nodesEl.querySelectorAll('.sc-node').forEach((el) => heights.set(el.dataset.node, el.offsetHeight));
    drawEdges();
  }
  const changed = () => { render(); o.onChange(script); };
  const select = (s) => { sel = s; render(); o.onSelect(sel); };

  // ------------------------------------------------------------------ editing
  function newId(type) {
    let i = 1;
    while (nodeById(type + i)) i++;
    return type + i;
  }
  function addNode(type, x, y) {
    if (type === 'start' && script.nodes.some((n) => n.type === 'start')) { o.onWarn('A script has only one Start block.'); return null; }
    const n = { id: newId(type), type, x: snap(x - widthOf(type) / 2), y: snap(y - (BLOCKS[type].tiny ? TINY / 2 : 20)), props: defaultProps(type) };
    script.nodes.push(n);
    sel = { kind: 'node', id: n.id };
    if (root.clientWidth < 600) togglePalette(false);  // a phone: the palette would hide the new block
    changed();
    o.onSelect(sel);
    return n;
  }
  // Puts a reroute point on an arrow: one arrow becomes two.
  function splitEdge(i, w) {
    const e = script.edges[i];
    if (!e) return;
    const n = { id: newId('reroute'), type: 'reroute', x: snap(w.x - TINY / 2), y: snap(w.y - TINY / 2), props: {} };
    script.nodes.push(n);
    script.edges.splice(i, 1, { from: e.from, port: e.port, to: n.id }, { from: n.id, port: 'out', to: e.to });
    sel = { kind: 'node', id: n.id };
    changed();
    o.onSelect(sel);
  }
  function defaultProps(type) {
    if (type === 'prompt') return { agent_id: '', project_id: '', text: '', target: '', retry: 0, retry_wait: 1 };
    if (type === 'ask') return { question: '', mode: 'approve', var: '', timeout: 0 };
    if (type === 'set') return { var: '', expr: '' };
    if (type === 'if') return { expr: '' };
    if (type === 'delay') return { mode: 'for', expr: '30' };
    if (type === 'do') return { count: '3', var: '' };
    return {};
  }
  function removeSelected() {
    if (o.readOnly || !sel) return;
    if (sel.kind === 'node') {
      script.nodes = script.nodes.filter((n) => n.id !== sel.id);
      script.edges = script.edges.filter((e) => e.from !== sel.id && e.to !== sel.id);
    } else {
      script.edges.splice(sel.index, 1);
    }
    sel = null;
    changed();
    o.onSelect(null);
  }
  function reaches(from, to) {  // is `to` reachable from `from`? (would a new arrow make a loop)
    const seen = new Set([from]), stack = [from];
    while (stack.length) {
      const id = stack.pop();
      if (id === to) return true;
      for (const e of script.edges) if (e.from === id && !seen.has(e.to)) { seen.add(e.to); stack.push(e.to); }
    }
    return false;
  }
  function connect(from, port, to, keepSelection) {
    const a = nodeById(from), b = nodeById(to);
    if (!a || !b || from === to) return;
    if (!BLOCKS[b.type]?.hasIn) { o.onWarn('Nothing can point to the Start block.'); return; }
    if (script.edges.some((e) => e.from === from && (e.port || 'out') === port && e.to === to)) return;
    if (reaches(to, from)) { o.onWarn('That arrow would make a loop (loops are not allowed).'); return; }
    script.edges.push({ from, port, to });
    if (keepSelection) { changed(); return; }
    sel = { kind: 'edge', index: script.edges.length - 1 };
    changed();
    o.onSelect(sel);
  }
  // An arrow drawn backwards (from an input): which output of the other block? With two outputs the person says.
  function connectFrom(from, to, cx, cy) {
    const outs = BLOCKS[nodeById(from)?.type]?.outs || [];
    if (from === to) return;
    if (!outs.length) { o.onWarn('The End block has no output.'); return; }
    if (outs.length === 1) { connect(from, outs[0], to); return; }
    showMenu(cx, cy, outs.map((p) => ({ id: 'port-' + p, label: PORT_MENU[p] || p, run: () => connect(from, p, to) })));
  }
  // An arrow dropped on empty space: the blocks it can lead to (or come from). Picking one adds it, already connected.
  function offerBlocks(g, cx, cy) {
    const w = toWorld(cx, cy), hasStart = script.nodes.some((n) => n.type === 'start');
    const fits = PALETTE.filter((t) => (g.to ? BLOCKS[t].outs.length > 0 && !(t === 'start' && hasStart) : BLOCKS[t].hasIn));
    showMenu(cx, cy, fits.map((t) => ({ id: 'new-' + t, label: BLOCKS[t].label, name: true, icon: BLOCKS[t].icon, run: () => {
      const tiny = BLOCKS[t].tiny;
      const n = addNode(t, w.x, tiny ? w.y : g.to ? w.y - 50 : w.y + 20);
      if (!n) return;
      if (g.to) connect(n.id, BLOCKS[t].outs[0], g.to, true);
      else connect(g.from, g.port, n.id, true);
    } })));
  }

  // Top to bottom: a block's row = the longest path from a block without inputs; rows ordered by their parents' places.
  function tidy() {
    const ids = script.nodes.map((n) => n.id);
    const parents = new Map(ids.map((id) => [id, []])), kids = new Map(ids.map((id) => [id, []]));
    for (const e of script.edges) if (parents.has(e.to) && kids.has(e.from)) { parents.get(e.to).push(e.from); kids.get(e.from).push(e.to); }
    const indeg = new Map(ids.map((id) => [id, parents.get(id).length]));
    const order = ids.filter((id) => indeg.get(id) === 0);
    for (let i = 0; i < order.length; i++) for (const k of kids.get(order[i])) { indeg.set(k, indeg.get(k) - 1); if (indeg.get(k) === 0) order.push(k); }
    for (const id of ids) if (!order.includes(id)) order.push(id);  // a loop: put the rest at the end
    const row = new Map();
    for (const id of order) row.set(id, Math.max(0, ...parents.get(id).map((p) => (row.get(p) ?? -1) + 1)));
    const rows = [];
    for (const id of order) (rows[row.get(id)] ||= []).push(id);
    const xOf = new Map();
    rows.forEach((r, ri) => {
      if (ri > 0) {
        const bary = (id) => { const ps = parents.get(id).filter((p) => xOf.has(p)); return ps.length ? ps.reduce((s, p) => s + xOf.get(p), 0) / ps.length : 0; };
        r.sort((a, b) => bary(a) - bary(b));
      }
      r.forEach((id, i) => xOf.set(id, (i - (r.length - 1) / 2) * (NODE_W + 50)));
    });
    for (const n of script.nodes) { n.x = snap(xOf.get(n.id) - widthOf(n.type) / 2); n.y = row.get(n.id) * 150; }
    changed();
    fit();
  }
  function fit() {
    const r = root.getBoundingClientRect();
    if (!script.nodes.length || !r.width) { pan = { x: r.width / 2, y: 60 }; zoom = 1; applyView(); return; }
    const xs = script.nodes.map((n) => n.x), ys = script.nodes.map((n) => n.y);
    const minX = Math.min(...xs) - 40, maxX = Math.max(...script.nodes.map((n) => n.x + widthOf(n.type))) + 40;
    const minY = Math.min(...ys) - 40, maxY = Math.max(...ys.map((y, i) => y + (heights.get(script.nodes[i].id) || 64))) + 40;
    zoom = Math.max(MIN_ZOOM, Math.min(1, r.width / (maxX - minX), (r.height - 50) / (maxY - minY)));
    pan = { x: (r.width - (maxX - minX) * zoom) / 2 - minX * zoom, y: 50 + ((r.height - 50) - (maxY - minY) * zoom) / 2 - minY * zoom };
    applyView();
  }
  function zoomAt(cx, cy, factor) {
    const r = root.getBoundingClientRect();
    const nz = Math.max(MIN_ZOOM, Math.min(MAX_ZOOM, zoom * factor));
    const px = cx - r.left, py = cy - r.top;
    pan = { x: px - (px - pan.x) * (nz / zoom), y: py - (py - pan.y) * (nz / zoom) };
    zoom = nz;
    applyView();
  }

  // ------------------------------------------------------------------ menus
  function showMenu(cx, cy, items) {
    const r = root.getBoundingClientRect();
    menu.innerHTML = items.map((it) => it ? `<button data-mi="${it.id}">${it.icon ? `<i>${it.icon}</i>` : ''}<span ${it.name ? 'data-no-i18n' : ''}>${esc(it.label)}</span></button>` : '<hr>').join('');
    menu.hidden = false;
    menu._items = items.filter(Boolean);
    const w = menu.offsetWidth, h = menu.offsetHeight;
    menu.style.left = Math.max(4, Math.min(cx - r.left, r.width - w - 4)) + 'px';
    menu.style.top = Math.max(4, Math.min(cy - r.top, r.height - h - 4)) + 'px';
  }
  const hideMenu = () => { menu.hidden = true; temp()?.setAttribute('d', ''); };  // the dashed arrow waits while the menu is open
  menu.addEventListener('click', (e) => {
    const b = e.target.closest('[data-mi]');
    if (!b) return;
    const it = menu._items.find((x) => x.id === b.dataset.mi);
    hideMenu();
    it?.run();
  });
  let longPressAt = 0;
  // When the finger of a long press lifts, the browser sends a click to the same spot, which is now under the menu:
  // it would pick the first item at once. Swallow that one click.
  let pressFired = false, swallowUntil = 0;
  root.addEventListener('click', (e) => {
    if (Date.now() < swallowUntil) { swallowUntil = 0; e.stopPropagation(); e.preventDefault(); }
  }, true);
  root.addEventListener('contextmenu', (e) => {
    e.preventDefault();
    if (Date.now() - longPressAt < 900) return;  // a touch browser's own long-press menu right after ours
    openContext(e.target, e.clientX, e.clientY);
  });
  function openContext(target, cx, cy) {
    if (o.readOnly) return;
    const w = toWorld(cx, cy);
    const nodeEl = target.closest('[data-node]'), edgeEl = target.closest('[data-edge]');
    const items = [];
    if (nodeEl) { select({ kind: 'node', id: nodeEl.dataset.node }); items.push({ id: 'del', label: 'Delete block', icon: '🗑', run: removeSelected }, null); }
    else if (edgeEl) {
      const index = +edgeEl.dataset.edge;
      select({ kind: 'edge', index });
      items.push({ id: 'bend', label: 'Add a reroute point here', icon: '•', run: () => splitEdge(index, w) }, { id: 'del', label: 'Delete arrow', icon: '🗑', run: removeSelected }, null);
    }
    for (const t of PALETTE) {
      if (t === 'start' && script.nodes.some((n) => n.type === 'start')) continue;
      items.push({ id: 'add-' + t, label: BLOCKS[t].label, name: true, icon: BLOCKS[t].icon, run: () => addNode(t, w.x, w.y) });
    }
    showMenu(cx, cy, items);
  }

  // ------------------------------------------------------------------ pointer
  const pointers = new Map();
  let gesture = null;  // {kind: 'pan'|'move'|'link'|'pinch'|'pal'|'edge', ...}
  let pressTimer = 0;
  const cancelPress = () => { clearTimeout(pressTimer); pressTimer = 0; };
  const temp = () => svg.querySelector('.sc-temp');

  root.addEventListener('pointerdown', (e) => {
    if (e.button === 2) return;
    if (e.target.closest('.sc-tools, .sc-menu, .sc-pal-head, .sc-pal-tip, .sc-banner')) return;
    hideMenu();
    pointers.set(e.pointerId, { x: e.clientX, y: e.clientY });
    if (pointers.size === 2) {  // pinch: two fingers zoom and pan
      cancelPress();
      const [a, b] = [...pointers.values()];
      gesture = { kind: 'pinch', d: Math.hypot(a.x - b.x, a.y - b.y), mx: (a.x + b.x) / 2, my: (a.y + b.y) / 2 };
      return;
    }
    const pal = e.target.closest('[data-add]');
    if (pal) { gesture = { kind: 'pal', type: pal.dataset.add, sx: e.clientX, sy: e.clientY, moved: false, ghost: null }; root.setPointerCapture(e.pointerId); return; }
    const out = e.target.closest('[data-out]'), inn = e.target.closest('[data-in]'), nodeEl = e.target.closest('[data-node]'), edgeEl = e.target.closest('[data-edge]');
    root.focus({ preventScroll: true });
    // a finger from the left edge pulls the palette out (phones have no room for a button bar)
    if (!o.readOnly && !nodeEl && !e.target.closest('.sc-palette') && e.clientX - root.getBoundingClientRect().left < 24) {
      gesture = { kind: 'edge', sx: e.clientX, sy: e.clientY, moved: false };
      root.setPointerCapture(e.pointerId);
      return;
    }
    // touch has no right click: holding a finger still opens the same menu
    if (e.pointerType !== 'mouse' && !o.readOnly && !out && !inn) {
      const cx = e.clientX, cy = e.clientY, target = e.target;
      cancelPress();
      pressTimer = setTimeout(() => {
        pressTimer = 0;
        if (!gesture || gesture.moved || pointers.size !== 1) return;
        gesture = null;
        longPressAt = Date.now();
        pressFired = true;
        openContext(target, cx, cy);
      }, 550);
    }
    if (!o.readOnly && pending?.to && nodeEl) {  // tap-to-connect from an input: this block leads to it
      const to = pending.to, from = nodeEl.dataset.node, port = out?.dataset.out;
      pending = null;
      render();
      if (port) connect(from, port, to); else connectFrom(from, to, e.clientX, e.clientY);
      gesture = null;
      return;
    }
    if (!o.readOnly && out) {
      gesture = { kind: 'link', from: nodeEl.dataset.node, port: out.dataset.out, sx: e.clientX, sy: e.clientY, moved: false };
    } else if (!o.readOnly && pending && (inn || nodeEl)) {  // tap-to-connect: the whole block is the target
      const p = pending;
      pending = null;
      connect(p.from, p.port, nodeEl.dataset.node);
      gesture = null;
      return;
    } else if (!o.readOnly && inn) {  // an arrow can also be drawn from the end it arrives at
      gesture = { kind: 'link', to: nodeEl.dataset.node, sx: e.clientX, sy: e.clientY, moved: false };
    } else if (nodeEl) {
      const n = nodeById(nodeEl.dataset.node);
      if (!(sel?.kind === 'node' && sel.id === n.id)) select({ kind: 'node', id: n.id });
      gesture = { kind: 'move', id: n.id, sx: e.clientX, sy: e.clientY, ox: n.x, oy: n.y, moved: false };
    } else if (edgeEl) {
      select({ kind: 'edge', index: +edgeEl.dataset.edge });
      gesture = null;
      return;
    } else {
      if (pending) { pending = null; render(); }
      gesture = { kind: 'pan', sx: e.clientX, sy: e.clientY, ox: pan.x, oy: pan.y, moved: false };
    }
    root.setPointerCapture(e.pointerId);
  });
  root.addEventListener('pointermove', (e) => {
    if (!pointers.has(e.pointerId)) return;
    pointers.set(e.pointerId, { x: e.clientX, y: e.clientY });
    if (!gesture) return;
    if (gesture.kind === 'pinch' && pointers.size === 2) {
      const [a, b] = [...pointers.values()];
      const d = Math.hypot(a.x - b.x, a.y - b.y), mx = (a.x + b.x) / 2, my = (a.y + b.y) / 2;
      pan = { x: pan.x + mx - gesture.mx, y: pan.y + my - gesture.my };
      zoomAt(mx, my, d / gesture.d);
      Object.assign(gesture, { d, mx, my });
      return;
    }
    const dx = e.clientX - gesture.sx, dy = e.clientY - gesture.sy;
    if (!gesture.moved && Math.hypot(dx, dy) < 6) return;
    gesture.moved = true;
    cancelPress();
    if (gesture.kind === 'edge') {
      if (dx > 40) { togglePalette(true); gesture = null; }
      return;
    }
    if (gesture.kind === 'pan') { pan = { x: gesture.ox + dx, y: gesture.oy + dy }; applyView(); }
    else if (gesture.kind === 'move' && !o.readOnly) {
      const n = nodeById(gesture.id);
      n.x = snap(gesture.ox + dx / zoom);
      n.y = snap(gesture.oy + dy / zoom);
      const el = nodesEl.querySelector(`[data-node="${CSS.escape(n.id)}"]`);
      if (el) { el.style.left = n.x + 'px'; el.style.top = n.y + 'px'; }
      drawEdges();
    } else if (gesture.kind === 'link') {
      const at = toWorld(e.clientX, e.clientY), n = nodeById(gesture.to || gesture.from);
      temp()?.setAttribute('d', gesture.to ? curve(at, portIn(n), false, isTiny(n)) : curve(portOut(n, gesture.port), at, isTiny(n), false));
    } else if (gesture.kind === 'pal') {
      if (!gesture.ghost) {
        gesture.ghost = document.createElement('div');
        gesture.ghost.className = 'sc-drag-ghost';
        gesture.ghost.textContent = `${BLOCKS[gesture.type].icon} ${BLOCKS[gesture.type].label}`;
        gesture.ghost.setAttribute('data-no-i18n', '');
        document.body.appendChild(gesture.ghost);
      }
      gesture.ghost.style.left = e.clientX + 'px';
      gesture.ghost.style.top = e.clientY + 'px';
    }
  });
  const endPointer = (e) => {
    cancelPress();
    if (pressFired) { pressFired = false; swallowUntil = Date.now() + 400; }  // the click of the lifting finger
    if (!pointers.has(e.pointerId)) return;
    pointers.delete(e.pointerId);
    const g = gesture;
    if (!g || (g.kind === 'pinch' && pointers.size)) return;
    gesture = null;
    if (g.kind === 'move' && g.moved) o.onChange(script);
    if ((g.kind === 'pan' || g.kind === 'edge') && !g.moved && sel) select(null);
    if (g.kind === 'link') {
      if (!g.moved) { pending = g.to ? { to: g.to } : { from: g.from, port: g.port }; render(); return; }  // tap: now tap the other block
      const hit = document.elementFromPoint(e.clientX, e.clientY), el = hit?.closest('[data-node]');
      if (hit && root.contains(hit) && !el && !hit.closest('.sc-tools, .sc-palette, .sc-menu, .sc-banner')) { offerBlocks(g, e.clientX, e.clientY); return; }
      temp()?.setAttribute('d', '');
      if (el && g.to) {
        const portEl = hit.closest('[data-out]');
        if (portEl) connect(el.dataset.node, portEl.dataset.out, g.to); else connectFrom(el.dataset.node, g.to, e.clientX, e.clientY);
      } else if (el && el.dataset.node !== g.from) connect(g.from, g.port, el.dataset.node);
    }
    if (g.kind === 'pal') {
      g.ghost?.remove();
      const r = root.getBoundingClientRect(), palEl = root.querySelector('.sc-palette');
      if (!g.moved) { const c = toWorld(r.left + r.width / 2, r.top + r.height / 2); addNode(g.type, c.x, c.y); return; }
      const overPal = palEl && palEl.classList.contains('open') && e.clientX < palEl.getBoundingClientRect().right;
      if (!overPal && e.clientX > r.left && e.clientX < r.right && e.clientY > r.top && e.clientY < r.bottom) {
        const w = toWorld(e.clientX, e.clientY);
        addNode(g.type, w.x, w.y);
      }
    }
  };
  root.addEventListener('pointerup', endPointer);
  root.addEventListener('pointercancel', (e) => { cancelPress(); pointers.delete(e.pointerId); gesture?.ghost?.remove(); gesture = null; temp()?.setAttribute('d', ''); });
  root.addEventListener('wheel', (e) => { e.preventDefault(); zoomAt(e.clientX, e.clientY, e.deltaY < 0 ? 1.1 : 1 / 1.1); }, { passive: false });
  root.addEventListener('keydown', (e) => {
    if (e.target !== root) return;
    if ((e.key === 'Delete' || e.key === 'Backspace') && sel) { e.preventDefault(); removeSelected(); }
    if (e.key === 'Escape') { pending = null; hideMenu(); render(); }
  });
  root.querySelector('.sc-tools').addEventListener('click', (e) => {
    const b = e.target.closest('[data-sc]');
    if (!b) return;
    const r = root.getBoundingClientRect();
    if (b.dataset.sc === 'zoomin') zoomAt(r.left + r.width / 2, r.top + r.height / 2, 1.25);
    else if (b.dataset.sc === 'zoomout') zoomAt(r.left + r.width / 2, r.top + r.height / 2, 0.8);
    else if (b.dataset.sc === 'fit') fit();
    else if (b.dataset.sc === 'tidy') tidy();
    else if (b.dataset.sc === 'palette') togglePalette();
  });
  root.querySelector('.sc-banner').addEventListener('click', (e) => { if (e.target.closest('[data-sc=cancel-link]')) { pending = null; render(); } });
  const togglePalette = (open) => root.querySelector('.sc-palette')?.classList.toggle('open', open);
  root.querySelector('.sc-pal-head')?.addEventListener('click', (e) => { if (e.target.closest('[data-sc]')) togglePalette(false); });

  render();
  requestAnimationFrame(fit);

  return {
    setScript(s) { script = s; render(); },
    refresh: render,
    setErrors(list) {
      errors = new Map();
      for (const er of list || []) if (er.node) (errors.get(er.node) || errors.set(er.node, []).get(er.node)).push(er.message);
      render();
    },
    setStates(map, edges) { states = new Map(Object.entries(map || {})); edgeStates = edges || []; render(); },
    select(s) { select(s); },
    selected: () => sel,
    remove: removeSelected,
    fit, tidy, togglePalette,
    destroy() { root.remove(); },
  };
}
