/* LegoToypad Remote - mobile-first phone controller.
   The desktop app stays the bridge: it embeds this UI, serves it over your
   LAN, and relays Load/Move/Clear to the emulator's Toypad listener. */

'use strict';

// --- layout constants (9:16 portrait stage, mirrored by style.css) --------
const STAGE_W = 405, STAGE_H = 720;

// pad cells in stage coordinates. Top row keeps the iconic 3/1/3 Toypad
// layout with the center pad dominant; the lower row carries 4 smaller pads.
const PAD_CELLS = [
  { x: 29,  y: 0,   w: 106, h: 244 }, // 0 Left - upper
  { x: 143, y: 0,   w: 120, h: 244 }, // 1 Center (large)
  { x: 271, y: 0,   w: 106, h: 244 }, // 2 Right - upper
  { x: 15,  y: 256, w: 86,  h: 188 }, // 3 Left - lower left
  { x: 111, y: 256, w: 86,  h: 188 }, // 4 Left - lower right
  { x: 207, y: 256, w: 86,  h: 188 }, // 5 Right - lower left
  { x: 303, y: 256, w: 86,  h: 188 }, // 6 Right - lower right
];

const FR_COLS = 2, RO_COLS = 3;

// --- app state -----------------------------------------------------------
let CAT = null;            // catalog from /api/catalog
let screen = 'pad';        // pad | franchise | roster | plus
let curSlot = null;        // selected pad slot (null = nothing selected)
let moveSource = -1;       // -1 = not picking a move destination
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
    pad.style.borderRadius = i === 1 ? '28px' : (i < 3 ? '20px' : '16px');
    pad.dataset.slot = i;

    const art = document.createElement('img');
    art.className = 'padart';
    art.src = CAT.pads[i];
    pad.appendChild(art);

    const dot = document.createElement('div');
    dot.className = 'padcdot';
    pad.appendChild(dot);

    const portrait = document.createElement('img');
    portrait.className = 'padportrait';
    portrait.alt = '';
    pad.appendChild(portrait);

    const name = document.createElement('div');
    name.className = 'padname';
    pad.appendChild(name);

    pad.addEventListener('click', () => onPadTap(i));
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
    const name = pad.querySelector('.padname');
    const dot = pad.querySelector('.padcdot');

    portrait.style.display = occupied ? 'block' : 'none';
    portrait.src = occupied ? p.portrait : '';
    const size = Math.round(Math.min(PAD_CELLS[i].w, PAD_CELLS[i].h) * 0.58);
    portrait.style.width = size + 'px';
    portrait.style.height = size + 'px';
    portrait.style.left = '50%';
    portrait.style.top = '50%';
    portrait.style.transform = 'translate(-50%, -50%)';
    portrait.style.border = occupied ? `3px solid ${p.color}` : 'none';
    portrait.style.boxShadow = occupied ? `0 0 16px ${rgba(p.color, 0.7)}` : 'none';

    name.textContent = occupied ? p.name : '';
    name.style.display = occupied ? 'block' : 'none';

    pad.classList.toggle('selected', i === curSlot);
    pad.classList.toggle('moveSource', i === moveSource);
    pad.style.border = (i === curSlot && curSlot !== null)
      ? '3px solid #E83838' : '2px solid transparent';

    const tint = occupied ? rgba(p.color, 0.18) : 'transparent';
    dot.style.background = occupied
      ? `radial-gradient(circle at 50% 45%, ${rgba(p.color, 0.30)} 0%, ${rgba(p.color, 0.10)} 70%, transparent 100%)`
      : 'none';
  }
  updateChip();
}

function updateChip() {
  const chip = $('chipText');
  if (moveSource >= 0) {
    chip.textContent = 'Pick the destination pad for PAD ' + (moveSource + 1);
    return;
  }
  if (curSlot === null) {
    chip.textContent = 'Tap a pad to select it';
    return;
  }
  const p = lastState && lastState.pads ? lastState.pads[curSlot] : null;
  if (p && p.occupied) {
    chip.textContent = `PAD ${curSlot + 1} — ${p.name}`;
  } else {
    chip.textContent = `PAD ${curSlot + 1} (empty)`;
  }
}

// --- action bar -----------------------------------------------------------
function buildActionBar() {
  const bar = $('actionBar');
  bar.textContent = '';
  const specs = [
    { img: CAT.loadBtn, label: 'LOAD' },
    { img: CAT.moveBtn, label: 'MOVE' },
    { img: CAT.clearBtn, label: 'CLEAR' },
  ];
  specs.forEach((s, i) => {
    const btn = document.createElement('div');
    btn.className = 'actbtn';
    const img = document.createElement('img');
    img.src = s.img;
    img.alt = s.label;
    const lbl = document.createElement('span');
    lbl.className = 'alabel';
    lbl.textContent = s.label;
    btn.appendChild(img);
    btn.appendChild(lbl);
    btn.addEventListener('click', () => onActionTap(i));
    bar.appendChild(btn);
  });
}

// --- franchise grid -------------------------------------------------------
function buildFranchiseGrid() {
  const grid = $('franchiseGrid');
  grid.textContent = '';
  CAT.franchises.forEach((world, idx) => {
    const tile = document.createElement('div');
    tile.className = 'fworld';
    tile.style.backgroundImage = `url(${CAT.worldTile})`;
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
}

function onPadTap(slot) {
  if (moveSource >= 0) {
    if (slot === moveSource) {
      moveSource = -1;
      refreshPadsFromState();
      showToast('Move cancelled.');
    } else {
      apiMove(moveSource, slot);
    }
    return;
  }
  if (curSlot === slot && lastState && lastState.pads && lastState.pads[slot].occupied) {
    setScreen('franchise');
    return;
  }
  curSlot = slot;
  refreshPadsFromState();
}

async function onActionTap(action) {
  if (action === 0) {           // Load
    if (curSlot === null) {
      showToast('Tap a pad first.');
      return;
    }
    setScreen('franchise');
  } else if (action === 1) {    // Move
    if (curSlot === null) {
      showToast('Tap a pad first.');
      return;
    }
    const st = await getState();
    if (!st || !st.pads[curSlot].occupied) {
      showToast('There is nothing tracked on that pad to move.');
      return;
    }
    moveSource = curSlot;
    refreshPadsFromState();
    showToast('Pick a destination pad.');
  } else {                      // Clear
    if (curSlot === null) {
      showToast('Tap a pad first.');
      return;
    }
    apiClear(curSlot);
  }
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
  moveSource = -1;
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
  } else if (screen === 'roster') {
    setScreen('franchise');
  } else if (screen === 'plus') {
    setScreen('roster');
  }
}

// --- boot -----------------------------------------------------------------
async function boot() {
  scaleStage();
  window.addEventListener('resize', scaleStage);
  $('backBtn').addEventListener('click', goBack);

  try {
    CAT = await api('/api/catalog');
  } catch (e) {
    $('statusbar').textContent = 'Cannot reach LegoToypad. Is the app running with the Web remote enabled?';
    $('connDot').classList.add('bad');
    return;
  }

  document.title = CAT.appName || 'LegoToypad Remote';

  // inject the bundled Compacta typeface; never block first paint on it
  if (CAT.fontUrl) {
    try {
      const f = new FontFace('UI Compacta', `url(${CAT.fontUrl})`);
      f.load().then(() => document.fonts.add(f)).catch(() => {});
    } catch (e) { /* fall back to system condensed fonts */ }
  }

  $('bgimg').src = CAT.background;
  $('wordmark').src = CAT.wordmark;

  buildPads();
  buildActionBar();
  buildFranchiseGrid();

  setScreen('pad');
  await getState();
  setInterval(getState, 3000);
}

boot();