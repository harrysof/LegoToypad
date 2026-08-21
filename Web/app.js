/* LegoToypad Remote - mobile-first phone controller.
   The desktop app stays the bridge: it embeds this UI, serves it over your
   LAN, and relays Load/Move/Clear to the emulator's Toypad listener. */

'use strict';

// --- layout constants (9:16 portrait stage, mirrored by style.css) --------
const STAGE_W = 405, STAGE_H = 720;

// Pad cells in stage coordinates, mirroring the real 3/1/3 Toypad shape:
// the inner lower pads (4 and 5) sit half a pad lower than the outer lower
// pads (3 and 6), so each side reads as a diagonal, like the physical pad.
const PAD_CELLS = [
  { x: 32,  y: 76,  w: 104, h: 104 }, // 0 Left - upper
  { x: 136, y: 34,  w: 134, h: 134 }, // 1 Center (large)
  { x: 269, y: 76,  w: 104, h: 104 }, // 2 Right - upper
  { x: 24,  y: 254, w: 84,  h: 84  }, // 3 Left - lower left (outer)
  { x: 116, y: 296, w: 84,  h: 84  }, // 4 Left - lower right (inner, lowered)
  { x: 208, y: 296, w: 84,  h: 84  }, // 5 Right - lower left (inner, lowered)
  { x: 300, y: 254, w: 84,  h: 84  }, // 6 Right - lower right (outer)
];

const FR_COLS = 2, RO_COLS = 3;

// --- app state -----------------------------------------------------------
let CAT = null;            // catalog from /api/catalog
let screen = 'pad';        // pad | franchise | roster | plus
let curSlot = null;        // selected pad slot (null = nothing selected)
let lastTapSlot = -1;      // double-tap detection
let lastTapTime = 0;
let dragState = null;      // active pad drag (null = none)
let suppressClick = false; // ignore a click right after a drag
let curWorld = null;       // franchise object for roster/plus
let curGroup = null;       // vehicle group object for the plus picker
let lastState = null;      // last /api/state payload

const $ = (id) => document.getElementById(id);
const pads = [];            // pad DOM elements, indexed by slot
let toastTimer = null;

// --- helpers -------------------------------------------------------------
function rgba(hex, a) {
  const r = parseInt(hex.slice(1, 3), 16);
  const g = parseInt(hex.slice(3, 5), 16);
  const b = parseInt(hex.slice(5, 7), 16);
  return `rgba(${r},${g},${b},${a})`;
}

function showToast(text) {
  const el = $('toast');
  el.textContent = text;
  el.classList.add('show');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => el.classList.remove('show'), 2600);
}

async function api(url, opts) {
  const res = await fetch(url, opts);
  if (!res.ok) throw new Error('HTTP ' + res.status);
  return res.json();
}

// --- drag & drop ----------------------------------------------------------
// Pointer-capture drag between pads. The finger never loses the gesture
// because the source pad grabs the pointer, and both the ghost and the
// possible drop target give live feedback so a move always "registers".
function toStage(e) {
  const rect = $('stage').getBoundingClientRect();
  const scale = rect.width / STAGE_W;
  return { x: (e.clientX - rect.left) / scale, y: (e.clientY - rect.top) / scale };
}

function hitTestPad(clientX, clientY) {
  for (let i = 0; i < pads.length; i++) {
    const r = pads[i].getBoundingClientRect();
    if (clientX >= r.left - 12 && clientX <= r.right + 12 && clientY >= r.top - 12 && clientY <= r.bottom + 12) return i;
  }
  return -1;
}

function startDrag(slot, e) {
  const pad = pads[slot];
  clearDropHighlight();
  dragState = { slot, startX: e.clientX, startY: e.clientY, active: false, warned: false };
  if (pad.classList.contains('occupied')) {
    try { pad.setPointerCapture(e.pointerId); } catch (err) {}
    const portrait = pad.querySelector('.padportrait');
    if (portrait && portrait.src) $('dragGhost').src = portrait.src;
    const st = toStage(e);
    const ghost = $('dragGhost');
    ghost.style.left = (st.x - 32) + 'px';
    ghost.style.top = (st.y - 32) + 'px';
    ghost.classList.add('active');
    pad.classList.add('dragging');
  }
}

function clearDropHighlight() {
  pads.forEach(p => p.classList.remove('dropTarget'));
}

function moveDrag(e) {
  if (!dragState) return;
  const dx = e.clientX - dragState.startX;
  const dy = e.clientY - dragState.startY;
  const ghost = $('dragGhost');
  if (!dragState.active && Math.hypot(dx, dy) > 6) {
    dragState.active = true;
    if (!pads[dragState.slot].classList.contains('occupied')) {
      dragState.warned = true;
      showToast('Nothing on this pad to move.');
    }
  }
  if (!dragState.active) return;
  const st = toStage(e);
  ghost.style.left = (st.x - 32) + 'px';
  ghost.style.top = (st.y - 32) + 'px';
  const over = hitTestPad(e.clientX, e.clientY);
  clearDropHighlight();
  if (over >= 0 && over !== dragState.slot) pads[over].classList.add('dropTarget');
}

function endDrag(e) {
  if (!dragState) return;
  const source = dragState.slot;
  const didDrag = dragState.active && pads[source].classList.contains('occupied');
  clearDropHighlight();
  pads[source].classList.remove('dragging');
  $('dragGhost').classList.remove('active');
  if (didDrag) {
    suppressClick = true;
    setTimeout(() => { suppressClick = false; }, 500);
    const drop = hitTestPad(e.clientX, e.clientY);
    if (drop >= 0 && drop !== source) apiMove(source, drop);
  }
  dragState = null;
}

function cancelDrag() {
  if (!dragState) return;
  clearDropHighlight();
  pads[dragState.slot].classList.remove('dragging');
  $('dragGhost').classList.remove('active');
  dragState = null;
}

window.addEventListener('pointermove', moveDrag);
window.addEventListener('pointerup', endDrag);
window.addEventListener('pointercancel', cancelDrag);

// --- boot -----------------------------------------------------------------
function scaleStage() {
  const vw = document.documentElement.clientWidth;
  const vh = window.innerHeight;
  const s = Math.min(vw / STAGE_W, vh / STAGE_H);
  $('stage').style.transform = `scale(${s})`;
}

// --- pad grid -------------------------------------------------------------
function buildPads() {
  const container = $('pads');
  container.textContent = '';
  for (let i = 0; i < 7; i++) {
    const c = PAD_CELLS[i];
    const pad = document.createElement('div');
    pad.className = 'pad';
    pad.style.left = c.x + 'px';
    pad.style.top = c.y + 'px';
    pad.style.width = c.w + 'px';
    pad.style.height = c.h + 'px';
    pad.style.borderRadius = '10px';
    pad.dataset.slot = i;

    const dot = document.createElement('div');
    dot.className = 'padcdot';
    pad.appendChild(dot);

    const portrait = document.createElement('img');
    portrait.className = 'padportrait';
    portrait.alt = '';
    pad.appendChild(portrait);

    pad.addEventListener('click', () => onPadTap(i));
    pad.addEventListener('pointerdown', (e) => startDrag(i, e));
    container.appendChild(pad);
    pads.push(pad);
  }
}

function refreshPads(es) {
  lastState = es;
  const padsData = es.pads;
  for (let i = 0; i < 7; i++) {
    const pad = pads[i];
    const p = padsData[i];
    const occupied = p.occupied;

    const portrait = pad.querySelector('.padportrait');
    const dot = pad.querySelector('.padcdot');

    portrait.style.display = occupied ? 'block' : 'none';
    portrait.src = occupied ? p.portrait : '';
    const size = Math.round(Math.min(PAD_CELLS[i].w, PAD_CELLS[i].h) * (i === 1 ? 0.56 : 0.62));
    portrait.style.width = size + 'px';
    portrait.style.height = size + 'px';
    portrait.style.left = '50%';
    portrait.style.top = '50%';
    portrait.style.transform = 'translate(-50%, -50%)';
    portrait.style.border = occupied ? `3px solid ${p.color}` : 'none';
    portrait.style.boxShadow = occupied ? `0 0 16px ${rgba(p.color, 0.7)}` : 'none';

    pad.classList.toggle('occupied', occupied);
    pad.classList.toggle('selected', i === curSlot);
    pad.style.border = (i === curSlot && curSlot !== null)
      ? '3px solid #E83838' : '2px solid transparent';

    const tint = occupied ? rgba(p.color, 0.18) : 'transparent';
    dot.style.background = occupied
      ? `radial-gradient(circle at 50% 45%, ${rgba(p.color, 0.30)} 0%, ${rgba(p.color, 0.10)} 70%, transparent 100%)`
      : 'none';
  }
  updateFloatName();
}

// Small faded caption floating 1px below the very top of the selected pad,
// just clearing the selection highlight, fading in and out on select.
function updateFloatName() {
  const el = $('floatName');
  if (screen !== 'pad' || curSlot === null) {
    el.classList.remove('show');
    return;
  }
  const p = lastState && lastState.pads ? lastState.pads[curSlot] : null;
  if (!p || !p.occupied || !p.name) {
    el.classList.remove('show');
    return;
  }
  el.textContent = p.name;
  const sr = $('stage').getBoundingClientRect();
  const scale = sr.width / STAGE_W;
  const r = pads[curSlot].getBoundingClientRect();
  const px = (r.left - sr.left) / scale;
  const py = (r.top - sr.top) / scale;
  const pw = r.width / scale;
  const w = el.offsetWidth || 120;
  const left = px + (pw - w) / 2;
  el.style.left = Math.max(4, left) + 'px';
  el.style.top = (py + 1) + 'px';
  el.classList.add('show');
}

// --- action bar -----------------------------------------------------------
function buildActionBar() {
  const bar = $('actionBar');
  bar.textContent = '';
  const btn = document.createElement('div');
  btn.className = 'actbtn';
  const img = document.createElement('img');
  img.src = CAT.clearBtn;
  img.alt = 'CLEAR';
  const lbl = document.createElement('span');
  lbl.className = 'alabel';
  lbl.textContent = 'CLEAR';
  btn.appendChild(img);
  btn.appendChild(lbl);
  btn.addEventListener('click', () => onActionTap(0));
  bar.appendChild(btn);
}

// --- franchise grid -------------------------------------------------------
function buildFranchiseGrid() {
  const grid = $('franchiseGrid');
  grid.textContent = '';
  CAT.franchises.forEach((world, idx) => {
    const tile = document.createElement('div');
    tile.className = 'fworld';
    const logo = document.createElement('img');
    logo.className = 'logo';
    logo.src = world.logo;
    logo.alt = world.name;
    tile.appendChild(logo);
    tile.addEventListener('click', () => onWorldTap(idx, tile));
    grid.appendChild(tile);
  });
  wireScroller(grid, $('franchiseScroll'));
}

// --- roster grid ----------------------------------------------------------
function buildRoster(world) {
  const grid = $('rosterGrid');
  grid.textContent = '';

  const chars = world.characters;
  const vehs = world.vehicles.length ? world.vehicles : [];

  chars.forEach((e) => grid.appendChild(makeFig(e)));

  if (chars.length && vehs.length) {
    const sep = document.createElement('div');
    sep.className = 'rowsep';
    grid.appendChild(sep);
  }

  vehs.forEach((group) => {
    grid.appendChild(makeFig(group.builds[0]));
    if (group.builds.length > 1) {
      grid.appendChild(makePlusFig(group));
    }
  });

  wireScroller(grid, $('rosterScroll'));
}

function makeFig(entry) {
  const fig = document.createElement('div');
  fig.className = 'fig';
  const ring = document.createElement('div');
  ring.className = 'ring bordered';
  ring.style.setProperty('--fig-color', entry.color);
  const hasPortrait = entry.portrait && !entry.portrait.endsWith('/0');
  if (hasPortrait) {
    const img = document.createElement('img');
    img.src = entry.portrait;
    img.alt = entry.name;
    img.style.boxShadow = `0 0 12px ${rgba(entry.color, 0.55)}`;
    ring.appendChild(img);
  } else {
    const letter = document.createElement('span');
    letter.className = 'letter';
    letter.textContent = (entry.name || '?').charAt(0);
    ring.appendChild(letter);
  }
  fig.appendChild(ring);
  const lbl = document.createElement('div');
  lbl.className = 'lbl';
  lbl.textContent = entry.name;
  fig.appendChild(lbl);

  fig.addEventListener('click', () => {
    highlightTouched(fig);
    apiLoad(curSlot, entry.bin);
  });
  return fig;
}

function makePlusFig(group) {
  const fig = document.createElement('div');
  fig.className = 'fig plus';
  const ring = document.createElement('div');
  ring.className = 'ring plus-ring';
  ring.style.setProperty('--fig-color', group.builds[0].color);
  ring.style.boxShadow = `0 0 12px ${rgba(group.builds[0].color, 0.4)}`;
  const span = document.createElement('span');
  span.textContent = '+';
  ring.appendChild(span);
  fig.appendChild(ring);
  const lbl = document.createElement('div');
  lbl.className = 'lbl';
  lbl.textContent = 'MORE';
  fig.appendChild(lbl);
  fig.addEventListener('click', () => openPlus(group));
  return fig;
}

// --- plus picker ----------------------------------------------------------
function buildPlus(group) {
  const grid = $('plusGrid');
  grid.textContent = '';
  $('plusPanel').style.backgroundImage = `url(${CAT.charactersTile})`;
  $('plusWorldLogo').src = curWorld.logo;

  group.builds.forEach((entry) => {
    const fig = makeFig(entry);
    grid.appendChild(fig);
  });

  const plus = document.createElement('div');
  plus.className = 'fig plus';
  const ring = document.createElement('div');
  ring.className = 'ring plus-ring';
  ring.style.setProperty('--fig-color', group.builds[0].color);
  const span = document.createElement('span');
  span.textContent = '+';
  ring.appendChild(span);
  plus.appendChild(ring);
  grid.appendChild(plus);
}

// --- scrollbars -----------------------------------------------------------
function wireScroller(scroller, sb) {
  sb.querySelector('img').src = CAT.scrollBar;
  const toggle = () => {
    const can = scroller.scrollHeight > scroller.clientHeight;
    sb.classList.toggle('visible', can);
    drawThumb(scroller, sb);
  };
  scroller.addEventListener('scroll', () => drawThumb(scroller, sb));
  window.addEventListener('orientationchange', () => setTimeout(toggle, 300));
  toggle();
}

function drawThumb(scroller, sb) {
  const img = sb.querySelector('img');
  const ratio = scroller.clientHeight / scroller.scrollHeight;
  const trackH = sb.clientHeight;
  const thumbH = Math.max(22, Math.round(trackH * ratio));
  const scrollable = scroller.scrollHeight - scroller.clientHeight;
  const frac = scrollable > 0 ? scroller.scrollTop / scrollable : 0;
  img.style.height = thumbH + 'px';
  img.style.top = Math.round((trackH - thumbH) * frac) + 'px';
}

// --- interaction ----------------------------------------------------------
function setScreen(s) {
  screen = s;
  $('padScreen').classList.toggle('active', s === 'pad');
  $('franchiseScreen').classList.toggle('active', s === 'franchise');
  $('rosterScreen').classList.toggle('active', s === 'roster');
  $('plusScreen').classList.toggle('active', s === 'plus');
  $('backBtn').classList.toggle('visible', s !== 'pad');
  $('floatName').classList.remove('show');
  if (s === 'pad') updateFloatName();
}

function onPadTap(slot) {
  if (suppressClick) { suppressClick = false; return; }
  const now = Date.now();
  const isDouble = slot === lastTapSlot && now - lastTapTime < 350;
  lastTapSlot = slot;
  lastTapTime = now;
  if (isDouble) {
    curSlot = slot;
    setScreen('franchise');
    return;
  }
  curSlot = slot;
  refreshPadsFromState();
}

async function onActionTap(action) {
  const btn = document.querySelector('#actionBar .actbtn');
  if (btn) {
    btn.classList.add('tapped');
    setTimeout(() => btn.classList.remove('tapped'), 220);
  }
  if (curSlot === null) {
    showToast('Tap a pad first.');
    return;
  }
  apiClear(curSlot);
}

function onWorldTap(idx, tile) {
  curWorld = CAT.franchises[idx];
  highlightTouched(tile);
  $('worldLogo').src = curWorld.logo;
  buildRoster(curWorld);
  setScreen('roster');
}

function openPlus(group) {
  curGroup = group;
  buildPlus(group);
  setScreen('plus');
}

function highlightTouched(el) {
  el.classList.add('touched');
  setTimeout(() => el.classList.remove('touched'), 260);
}

// --- API ------------------------------------------------------------------
async function getState() {
  try {
    const s = await api('/api/state');
    $('connDot').classList.add('ok');
    $('connDot').classList.remove('bad');
    if (s && s.pads) refreshPads(s);
    if (s && s.status) $('statusbar').textContent = s.status;
    return s;
  } catch (e) {
    $('connDot').classList.add('bad');
    $('connDot').classList.remove('ok');
    return null;
  }
}

async function post(path, body, label) {
  try {
    const res = await fetch(path, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    });
    const data = await res.json();
    if (!data.ok) throw new Error(data.status || 'command failed');
    showToast(label + (data.status ? ': ' + data.status : ''));
    $('statusbar').textContent = data.status || '';
    await getState();
  } catch (e) {
    showToast(e.message || 'Could not reach the desktop app.');
    setScreen('pad');
  }
}

function apiLoad(slot, bin) {
  if (slot === null) { showToast('Tap a pad first.'); return; }
  if (bin == null) { showToast('This entry has no tag data.'); return; }
  post('/api/load', { slot: Number(slot), bin: Number(bin) }, 'LOAD');
  setScreen('pad');
}

function apiMove(src, dest) {
  post('/api/move', { src: Number(src), dest: Number(dest) }, 'MOVE');
  setScreen('pad');
}

function apiClear(slot) {
  post('/api/clear', { slot: Number(slot) }, 'CLEAR');
  setScreen('pad');
}

function refreshPadsFromState() {
  getState().then(s => {
    if (s && s.pads) refreshPads(s);
  });
}

// --- navigation -----------------------------------------------------------
function goBack() {
  if (screen === 'pad') return;
  if (screen === 'franchise') {
    setScreen('pad');
    refreshPadsFromState();
  } else if (screen === 'roster') {
    setScreen('franchise');
  } else if (screen === 'plus') {
    setScreen('roster');
  }
}

// --- boot -----------------------------------------------------------------
function setLoading(visible, text) {
  const l = $('loading');
  if (visible) {
    l.classList.remove('hidden');
    if (text) $('loadingText').textContent = text;
  } else {
    l.classList.add('hidden');
  }
}

async function boot() {
  scaleStage();
  window.addEventListener('resize', scaleStage);
  $('backBtn').addEventListener('click', goBack);

  setLoading(true, 'Connecting to LegoToypad…');
  try {
    CAT = await api('/api/catalog');
  } catch (e) {
    $('connDot').classList.add('bad');
    setLoading(true, 'Cannot reach LegoToypad.\nIs the app running with the Web remote enabled?');
    return;
  }

  $('loadingLogo').src = CAT.wordmark;
  document.title = CAT.appName || 'LegoToypad Remote';

  $('bgimg').src = CAT.background;
  $('wordmark').src = CAT.wordmark;

  buildPads();
  buildActionBar();
  buildFranchiseGrid();

  setScreen('pad');
  await getState();
  setTimeout(() => setLoading(false), 350);
  setInterval(getState, 3000);
}

boot();
