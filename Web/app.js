/* LegoToypad Remote - mobile-first and fully responsive controller UI.
   The desktop app stays the bridge: it embeds this UI, serves it over your
   LAN, and relays Load/Move/Clear to the emulator's Toypad listener. */

'use strict';

// Landscape layout: L-shape left (0, 3, 4), center circle (1), inverted-L right (2, 5, 6)
// Percentages are of the deck container (aspect-ratio: 520 / 290, R = 1.793)
// Wide, spread-apart portal layout with no cropping!
const PAD_CELLS_LANDSCAPE = [
  { x: 4.0,   y: 12.0, w: 18.0, h: 34.0 },  // 0 Left - upper
  { x: 37.5,  y: 8.0,  w: 25.0, h: 44.8 },  // 1 Center (circle)
  { x: 78.0,  y: 12.0, w: 18.0, h: 34.0 },  // 2 Right - upper
  { x: 4.0,   y: 54.0, w: 18.0, h: 36.0 },  // 3 Left - lower outer
  { x: 24.5,  y: 54.0, w: 18.0, h: 36.0 },  // 4 Left - lower inner
  { x: 57.5,  y: 54.0, w: 18.0, h: 36.0 },  // 5 Right - lower inner
  { x: 78.0,  y: 54.0, w: 18.0, h: 36.0 },  // 6 Right - lower outer
];

// Portrait layout: circle top → 2 upper pads → 4 lower pads (1 + 2 + 4 rows)
// Percentages are of the deck container (aspect-ratio: 350 / 400, R = 0.875)
// Spans edge-to-edge (2% to 98%) to eliminate side gaps and maximize pad touch targets!
const PAD_CELLS_PORTRAIT = [
  { x: 2.5,   y: 39.5, w: 45.5,  h: 28.0 }, // 0 Left upper  (row 2)
  { x: 30.0,  y: 2.5,  w: 40.0,  h: 35.0 }, // 1 Center circle (row 1, top)
  { x: 52.0,  y: 39.5, w: 45.5,  h: 28.0 }, // 2 Right upper (row 2)
  { x: 2.0,   y: 69.5, w: 22.65, h: 27.5 }, // 3 Left lower outer  (row 3)
  { x: 26.45, y: 69.5, w: 22.65, h: 27.5 }, // 4 Left lower inner  (row 3)
  { x: 50.9,  y: 69.5, w: 22.65, h: 27.5 }, // 5 Right lower inner (row 3)
  { x: 75.35, y: 69.5, w: 22.65, h: 27.5 }, // 6 Right lower outer (row 3)
];

// Media query that matches portrait orientation (width <= height)
const portraitMQ = window.matchMedia('(max-aspect-ratio: 1/1)');

function getPadCells() {
  return portraitMQ.matches ? PAD_CELLS_PORTRAIT : PAD_CELLS_LANDSCAPE;
}

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
let currentBg = null;      // current wallpaper background URL

const $ = (id) => document.getElementById(id);
const pads = [];            // pad DOM elements, indexed by slot

// --- helpers -------------------------------------------------------------
function rgba(hex, a) {
  const r = parseInt(hex.slice(1, 3), 16);
  const g = parseInt(hex.slice(3, 5), 16);
  const b = parseInt(hex.slice(5, 7), 16);
  return `rgba(${r},${g},${b},${a})`;
}

function getPadName(slot) {
  const names = [
    'left upper pad',
    'center pad',
    'right upper pad',
    'left lower outer pad',
    'left lower inner pad',
    'right lower inner pad',
    'right lower outer pad'
  ];
  return names[slot] || `pad ${slot}`;
}

function setStatusMessage(text, type = 'info') {
  const el = $('statusMessage');
  if (!el) return;
  el.textContent = text;
  el.className = 'status-message active ' + type;
  el.classList.remove('pulse');
  void el.offsetWidth; // trigger reflow for animation restart
  el.classList.add('pulse');
}

async function api(url, opts) {
  const res = await fetch(url, opts);
  if (!res.ok) throw new Error('HTTP ' + res.status);
  return res.json();
}

// --- drag & drop ----------------------------------------------------------
function hitTestPad(clientX, clientY) {
  for (let i = 0; i < pads.length; i++) {
    const r = pads[i].getBoundingClientRect();
    if (clientX >= r.left - 8 && clientX <= r.right + 8 && clientY >= r.top - 8 && clientY <= r.bottom + 8) return i;
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
    const ghost = $('dragGhost');
    ghost.style.left = (e.clientX - 32) + 'px';
    ghost.style.top = (e.clientY - 32) + 'px';
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
      setStatusMessage('Nothing on this pad to move.', 'warn');
    }
  }
  if (!dragState.active) return;
  ghost.style.left = (e.clientX - 32) + 'px';
  ghost.style.top = (e.clientY - 32) + 'px';
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

// --- pad grid -------------------------------------------------------------
function buildPads() {
  const container = $('pads');
  container.textContent = '';
  pads.length = 0;
  const cells = getPadCells();
  for (let i = 0; i < 7; i++) {
    const c = cells[i];
    const pad = document.createElement('div');
    pad.className = 'pad' + (i === 1 ? ' center-pad' : '');
    pad.style.left = c.x + '%';
    pad.style.top = c.y + '%';
    pad.style.width = c.w + '%';
    pad.style.height = c.h + '%';
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

// Update pad positions when orientation changes without rebuilding DOM
function repositionPads() {
  const cells = getPadCells();
  for (let i = 0; i < 7; i++) {
    const c = cells[i];
    pads[i].style.left    = c.x + '%';
    pads[i].style.top     = c.y + '%';
    pads[i].style.width   = c.w + '%';
    pads[i].style.height  = c.h + '%';
  }
  updateFloatName();
}

// --- portrait auto-crop cache ---------------------------------------------
const portraitCropCache = new Map();

function applyCroppedPortrait(imgElement, url) {
  if (!url || url.endsWith('/0')) {
    imgElement.src = url || '';
    return;
  }
  if (portraitCropCache.has(url)) {
    imgElement.src = portraitCropCache.get(url);
    return;
  }
  imgElement.dataset.rawSrc = url;
  imgElement.src = url;

  const raw = new Image();
  raw.crossOrigin = 'anonymous';
  raw.onload = () => {
    try {
      const nw = raw.naturalWidth || 512;
      const nh = raw.naturalHeight || 512;
      const pw = Math.min(nw, 256);
      const ph = Math.min(nh, 256);
      const probeCanvas = document.createElement('canvas');
      probeCanvas.width = pw;
      probeCanvas.height = ph;
      const pctx = probeCanvas.getContext('2d', { willReadFrequently: true });
      pctx.drawImage(raw, 0, 0, pw, ph);
      const data = pctx.getImageData(0, 0, pw, ph).data;

      let minX = pw, minY = ph, maxX = -1, maxY = -1;
      for (let y = 0; y < ph; y++) {
        const rowOffset = y * pw * 4;
        for (let x = 0; x < pw; x++) {
          if (data[rowOffset + x * 4 + 3] > 24) {
            if (x < minX) minX = x;
            if (x > maxX) maxX = x;
            if (y < minY) minY = y;
            if (y > maxY) maxY = y;
          }
        }
      }

      if (maxX < minX || maxY < minY) {
        portraitCropCache.set(url, url);
        return;
      }

      const scaleX = nw / pw;
      const scaleY = nh / ph;
      const srcX = Math.max(0, Math.floor(minX * scaleX));
      const srcY = Math.max(0, Math.floor(minY * scaleY));
      const srcW = Math.min(nw - srcX, Math.ceil((maxX - minX + 1) * scaleX));
      const srcH = Math.min(nh - srcY, Math.ceil((maxY - minY + 1) * scaleY));

      const outDim = 256;
      const outCanvas = document.createElement('canvas');
      outCanvas.width = outDim;
      outCanvas.height = outDim;
      const octx = outCanvas.getContext('2d');
      // Scale art to fill 94% of the circular frame
      const targetDim = outDim * 0.94;
      const fitScale = Math.min(targetDim / srcW, targetDim / srcH);
      const drawW = srcW * fitScale;
      const drawH = srcH * fitScale;
      const drawX = (outDim - drawW) / 2;
      const drawY = (outDim - drawH) / 2;

      octx.imageSmoothingEnabled = true;
      octx.imageSmoothingQuality = 'high';
      octx.drawImage(raw, srcX, srcY, srcW, srcH, drawX, drawY, drawW, drawH);

      const croppedUrl = outCanvas.toDataURL('image/png');
      portraitCropCache.set(url, croppedUrl);
      if (imgElement.dataset.rawSrc === url) {
        imgElement.src = croppedUrl;
      }
    } catch {
      portraitCropCache.set(url, url);
    }
  };
  raw.onerror = () => {
    portraitCropCache.set(url, url);
  };
  raw.src = url;
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
    if (occupied) {
      applyCroppedPortrait(portrait, p.portrait);
    } else {
      portrait.src = '';
    }
    portrait.style.border = occupied ? `3px solid ${p.color}` : 'none';
    portrait.style.boxShadow = occupied ? `0 0 16px ${rgba(p.color, 0.7)}` : 'none';

    pad.classList.toggle('occupied', occupied);
    pad.classList.toggle('selected', i === curSlot);
    pad.style.borderColor = (i === curSlot && curSlot !== null) ? '#E83838' : 'rgba(255, 255, 255, 0.22)';

    dot.style.background = occupied
      ? `radial-gradient(circle at 50% 45%, ${rgba(p.color, 0.30)} 0%, ${rgba(p.color, 0.10)} 70%, transparent 100%)`
      : 'none';
  }
  updateFloatName();
}

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
  const r = pads[curSlot].getBoundingClientRect();
  const w = el.offsetWidth || 110;
  const left = r.left + (r.width - w) / 2;
  el.style.left = Math.max(8, left) + 'px';
  el.style.top = Math.max(8, r.top - 28) + 'px';
  el.classList.add('show');
}

// --- action bar -----------------------------------------------------------
function makeActionButton(iconUrl, label, onTap) {
  const btn = document.createElement('div');
  btn.className = 'actbtn';
  const img = document.createElement('img');
  img.src = iconUrl;
  img.alt = label;
  const lbl = document.createElement('span');
  lbl.className = 'alabel';
  lbl.textContent = label;
  btn.appendChild(img);
  btn.appendChild(lbl);
  btn.addEventListener('click', () => onTap(btn));
  return btn;
}

function buildActionBar() {
  const bar = $('actionBar');
  bar.textContent = '';
  bar.appendChild(makeActionButton(CAT.clearBtn, 'CLEAR', onClearTap));
  // Favorites are added/removed from whatever is loaded on the selected
  // pad, next to Clear - not by holding a roster tile.
  bar.appendChild(makeActionButton(CAT.favoritesIcon, 'FAVORITE', onFavoriteTap));
}

// --- franchise grid -------------------------------------------------------
function buildFranchiseGrid() {
  const grid = $('franchiseGrid');
  grid.textContent = '';

  // Favorites tile, first in the grid - same custom_bin.png icon the
  // desktop overlay's own Favorites tile uses.
  const favTile = document.createElement('div');
  favTile.className = 'fworld';
  const favLogo = document.createElement('img');
  favLogo.className = 'logo';
  favLogo.src = CAT.favoritesIcon;
  favLogo.alt = 'Favorites';
  favTile.appendChild(favLogo);
  favTile.addEventListener('click', () => onFavoritesTap(favTile));
  grid.appendChild(favTile);

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

  // Only the default (build 1) tile is shown per vehicle, same as the
  // desktop overlay; its alternates are revealed through the build picker
  // when the tile itself is pressed, so there's no separate "+" grid slot.
  vehs.forEach((group) => {
    const entry = group.builds[0];
    grid.appendChild(makeFig(entry, group.builds.length > 1 ? group : null));
  });

  wireScroller(grid, $('rosterScroll'));
}

function makeFig(entry, group) {
  const fig = document.createElement('div');
  fig.className = 'fig';
  const ring = document.createElement('div');
  ring.className = 'ring bordered';
  ring.style.setProperty('--fig-color', entry.color);
  const hasPortrait = entry.portrait && !entry.portrait.endsWith('/0');
  if (hasPortrait) {
    const img = document.createElement('img');
    img.alt = entry.name;
    img.style.boxShadow = `0 0 12px ${rgba(entry.color, 0.55)}`;
    applyCroppedPortrait(img, entry.portrait);
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
    if (group) {
      openPlus(group);
    } else {
      apiLoad(curSlot, entry.bin, entry.name);
    }
  });
  return fig;
}

// --- plus picker ----------------------------------------------------------
function buildPlus(group) {
  const grid = $('plusGrid');
  grid.textContent = '';
  $('plusPanel').style.backgroundImage = `url(${CAT.charactersTile})`;
  $('plusWorldLogo').src = curWorld.logo;

  group.builds.forEach((entry) => {
    grid.appendChild(makeFig(entry));
  });
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
  window.addEventListener('resize', toggle);
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

function tapAnimate(btn) {
  if (!btn) return;
  btn.classList.add('tapped');
  setTimeout(() => btn.classList.remove('tapped'), 220);
}

function onClearTap(btn) {
  tapAnimate(btn);
  if (curSlot === null) {
    setStatusMessage('Tap a pad first to select a slot.', 'warn');
    return;
  }
  apiClear(curSlot);
}

// Favorites the character/vehicle currently loaded on the selected pad.
// Toggles: pressing it again on an already-favorited figure removes it.
async function onFavoriteTap(btn) {
  tapAnimate(btn);
  if (curSlot === null) {
    setStatusMessage('Tap a pad first to select a slot.', 'warn');
    return;
  }
  const pad = lastState && lastState.pads ? lastState.pads[curSlot] : null;
  if (!pad || !pad.occupied || !pad.bin) {
    setStatusMessage('Nothing on this pad to favorite.', 'warn');
    return;
  }
  let data;
  try {
    const res = await fetch('/api/favorite', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ bin: pad.bin }),
    });
    data = await res.json();
  } catch (e) {
    setStatusMessage('Could not reach the desktop app.', 'error');
    return;
  }
  if (!data.ok) {
    setStatusMessage('Could not update favorites.', 'error');
    return;
  }
  setStatusMessage(
    data.favorited ? `Added "${pad.name}" to favorites` : `Removed "${pad.name}" from favorites`,
    'success'
  );
}

function onWorldTap(idx, tile) {
  curWorld = CAT.franchises[idx];
  highlightTouched(tile);
  $('worldLogo').src = curWorld.logo;
  buildRoster(curWorld);
  setScreen('roster');
}

async function onFavoritesTap(tile) {
  highlightTouched(tile);
  let world;
  try {
    world = await api('/api/favorites');
  } catch (e) {
    setStatusMessage('Could not load favorites.', 'error');
    return;
  }
  curWorld = world;
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
    if (s && s.pads) refreshPads(s);
    if (s && s.background && s.background !== currentBg) {
      currentBg = s.background;
      const bg = $('bgimg');
      if (bg) bg.src = s.background;
    }
    return s;
  } catch (e) {
    return null;
  }
}

async function post(path, body, successMsg) {
  try {
    const res = await fetch(path, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    });
    const data = await res.json();
    if (!data.ok) throw new Error(data.status || 'command failed');
    if (successMsg) {
      setStatusMessage(successMsg, 'success');
    }
    await getState();
  } catch (e) {
    setStatusMessage(e.message || 'Could not reach the desktop app.', 'error');
    setScreen('pad');
  }
}

function apiLoad(slot, bin, name) {
  if (slot === null) {
    setStatusMessage('Tap a pad first to select a slot.', 'warn');
    return;
  }
  if (bin == null) {
    setStatusMessage('This entry has no tag data.', 'warn');
    return;
  }
  const padName = getPadName(slot);
  const charName = name || 'item';
  post('/api/load', { slot: Number(slot), bin: Number(bin) }, `loaded "${charName}" into ${padName}`);
  setScreen('pad');
}

function apiMove(src, dest) {
  const srcName = getPadName(src);
  const destName = getPadName(dest);
  post('/api/move', { src: Number(src), dest: Number(dest) }, `moved from ${srcName} to ${destName}`);
  setScreen('pad');
}

function apiClear(slot) {
  if (slot === null) {
    setStatusMessage('Tap a pad first to select a slot.', 'warn');
    return;
  }
  const padName = getPadName(slot);
  post('/api/clear', { slot: Number(slot) }, `cleared ${padName}`);
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
  // Reposition pads whenever the screen orientation crosses square threshold
  portraitMQ.addEventListener('change', () => {
    repositionPads();
    updateFloatName();
  });
  window.addEventListener('resize', updateFloatName);
  window.addEventListener('orientationchange', () => setTimeout(() => {
    repositionPads();
    updateFloatName();
  }, 200));
  $('backBtn').addEventListener('click', goBack);

  setLoading(true, 'Connecting to LegoToypad…');
  try {
    CAT = await api('/api/catalog');
  } catch (e) {
    setLoading(true, 'Cannot reach LegoToypad.\nIs the app running with the Web remote enabled?');
    return;
  }

  $('loadingLogo').src = CAT.wordmark;
  document.title = CAT.appName || 'LegoToypad Remote';

  currentBg = CAT.background;
  $('bgimg').src = CAT.background;
  $('wordmark').src = CAT.wordmark;
  if ($('byHarrysof') && CAT.byMark) $('byHarrysof').src = CAT.byMark;

  buildPads();
  buildActionBar();
  buildFranchiseGrid();

  setScreen('pad');
  await getState();
  setStatusMessage('Tap a pad to select, double-tap to browse characters');
  setTimeout(() => setLoading(false), 350);
  setInterval(getState, 3000);
}

boot();
