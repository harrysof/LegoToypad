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

// Toypad LED mirror: which pad slots each of the 3 physical LED regions
// covers - must match kLedRegionSlots in main.cpp (0=left, 1=center, 2=right).
const LED_REGION_SLOTS = [
  [0, 3, 4], // left
  [1],       // center
  [2, 5, 6], // right
];

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

// Franchise sort ids - must match AppState::FranchiseSort in main.cpp.
const SORT_DEFAULT = 0, SORT_USER = 1, SORT_STORY = 2, SORT_FAVORITES = 3,
      SORT_YEAR1 = 4, SORT_YEAR2 = 5, SORT_ABILITIES = 6;

let curSort = SORT_DEFAULT; // current franchise sort (mirrors the desktop)
let userOrder = [];         // franchise indices in the user's custom order
let abilityFilter = 0;      // 0 = All, else index into CAT.abilitySections + 1
let plusReturn = 'roster';  // screen the build picker goes back to
let selection = { world: -1, virtual: '', bin: 0, ability: -1 }; // desktop cursor
let selectionTimer = null;  // fast poll for the desktop cursor while browsing

const $ = (id) => document.getElementById(id);
const pads = [];            // pad DOM elements, indexed by slot

// --- helpers -------------------------------------------------------------
function rgba(hex, a) {
  const r = parseInt(hex.slice(1, 3), 16);
  const g = parseInt(hex.slice(3, 5), 16);
  const b = parseInt(hex.slice(5, 7), 16);
  return `rgba(${r},${g},${b},${a})`;
}

// True when a resource URL points at real art. Resource URLs carry a ?v=
// cache-bust token, so test the numeric id rather than the string suffix.
function hasArt(url) {
  return !!url && !/\/0(\?|$)/.test(url);
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

    const led = document.createElement('div');
    led.className = 'padled';
    pad.appendChild(led);

    const dot = document.createElement('div');
    dot.className = 'padcdot';
    pad.appendChild(dot);

    const ledTint = document.createElement('div');
    ledTint.className = 'padledtint';
    pad.appendChild(ledTint);

    const portrait = document.createElement('img');
    portrait.className = 'padportrait';
    portrait.alt = '';
    pad.appendChild(portrait);

    pad.addEventListener('click', () => onPadTap(i));
    pad.addEventListener('pointerdown', (e) => startDrag(i, e));
    container.appendChild(pad);
    pads.push(pad);
  }
  squareCenterPad();
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
  squareCenterPad();
  updateFloatName();
}

// The centre pad is a circle, but its cell is only square at the deck's
// designed aspect ratio. On a big TV/tablet the deck can get clamped wider
// than its aspect (see .pad-deck-card), squashing the cell into an ellipse.
// Pin slot 1 to an explicit square (the smaller of the cell's two sides,
// centred on the cell) so it stays round at any screen size.
function squareCenterPad() {
  const pad = pads[1];
  const deck = $('pads');
  if (!pad || !deck) return;
  const deckW = deck.clientWidth;
  const deckH = deck.clientHeight;
  if (!deckW || !deckH) return;
  const c = getPadCells()[1];
  const cellW = (c.w / 100) * deckW;
  const cellH = (c.h / 100) * deckH;
  const side = Math.min(cellW, cellH);
  const cx = ((c.x + c.w / 2) / 100) * deckW;
  const cy = ((c.y + c.h / 2) / 100) * deckH;
  pad.style.left = (cx - side / 2) + 'px';
  pad.style.top = (cy - side / 2) + 'px';
  pad.style.width = side + 'px';
  pad.style.height = side + 'px';
}

// --- portrait auto-crop cache ---------------------------------------------
const portraitCropCache = new Map();

function applyCroppedPortrait(imgElement, url) {
  if (!hasArt(url)) {
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
  // CLEAR: icon only (the broom art reads on its own), drawn larger.
  const clearBtn = makeActionButton(CAT.clearBtn, 'CLEAR', onClearTap);
  clearBtn.classList.add('icon-only', 'big-icon');
  bar.appendChild(clearBtn);
  // Favorites are added/removed from whatever is loaded on the selected
  // pad, next to Clear - not by holding a roster tile. Icon only, like Clear.
  const favBtn = makeActionButton(CAT.favoritesIcon, 'FAVORITE', onFavoriteTap);
  favBtn.classList.add('icon-only', 'big-icon');
  bar.appendChild(favBtn);
  // Abilities: shows whatever is loaded on the selected pad. Uses the app's
  // own "Abilities" branding logo (the sort badge art), so let it run wide.
  const abilitiesSort = (CAT.sorts || []).find((s) => s.id === SORT_ABILITIES);
  const abilitiesLogo = (abilitiesSort && hasArt(abilitiesSort.icon))
    ? abilitiesSort.icon : CAT.abilitiesTile;
  const abilitiesBtn = makeActionButton(abilitiesLogo, 'ABILITIES', onAbilitiesTap);
  abilitiesBtn.classList.add('wide-icon', 'icon-only');
  bar.appendChild(abilitiesBtn);
}

// --- browse screen (franchise / story / favorites / ability grids) --------
// One screen drives every "sort page": the world tile grid, the Starter Pack
// roster, the Favorites roster and the Abilities grid all render into
// #franchiseGrid, so the sort switcher stays put while you cycle them.
function sortInfo(id) {
  const list = (CAT && CAT.sorts) || [];
  return list.find((s) => s.id === id) ||
    { id, label: 'Default', icon: '', color: '#F0F4FA' };
}

function updateSortBar() {
  const info = sortInfo(curSort);
  const icon = $('sortIcon');
  const badge = $('sortBadge');
  // The badge art already spells out the sort name, so show it alone (no
  // separate text label).
  if (icon) {
    const hasIcon = hasArt(info.icon);
    icon.style.display = hasIcon ? '' : 'none';
    if (hasIcon) icon.src = info.icon;
  }
  if (badge) {
    const color = info.color || '#49B7FF';
    badge.style.setProperty('--sort-color', color);
    badge.style.setProperty('--sort-glow', rgba(color, 0.34));
  }
  const bar = $('abilityFilterBar');
  if (bar) bar.classList.toggle('visible', curSort === SORT_ABILITIES);
}

function buildAbilityFilterBar() {
  const bar = $('abilityFilterBar');
  bar.textContent = '';
  const sections = ['All'].concat(CAT.abilitySections || []);
  sections.forEach((name, i) => {
    const chip = document.createElement('div');
    chip.className = 'abfilter' + (i === abilityFilter ? ' active' : '');
    chip.textContent = name;
    chip.addEventListener('click', () => {
      abilityFilter = i;
      updateAbilityFilterBar();
      if (screen === 'franchise' && curSort === SORT_ABILITIES) renderBrowse();
    });
    bar.appendChild(chip);
  });
}

function updateAbilityFilterBar() {
  const bar = $('abilityFilterBar');
  if (!bar) return;
  [...bar.children].forEach((chip, i) => chip.classList.toggle('active', i === abilityFilter));
}

function cycleSort(dir) {
  const list = (CAT && CAT.sorts) || [];
  if (!list.length) return;
  let i = list.findIndex((s) => s.id === curSort);
  if (i < 0) i = 0;
  changeSort(list[(i + dir + list.length) % list.length].id);
}

async function changeSort(id) {
  const previous = curSort;
  curSort = id;
  try {
    const res = await fetch('/api/sort', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ sort: Number(id) }),
    });
    const data = await res.json();
    if (data && data.ok) {
      curSort = data.sort;
      if (Array.isArray(data.userOrder)) userOrder = data.userOrder;
    }
  } catch (e) {
    curSort = previous;
    setStatusMessage('Could not change the sort.', 'error');
  }
  renderBrowse();
}

async function refreshSortData() {
  try {
    const data = await api('/api/sort');
    if (data && typeof data.sort === 'number') curSort = data.sort;
    if (data && Array.isArray(data.userOrder)) userOrder = data.userOrder;
  } catch (e) { /* keep whatever we had */ }
}

async function renderBrowse() {
  const grid = $('franchiseGrid');
  if (!grid || !CAT) return;
  grid.scrollTop = 0;
  grid.className = '';
  grid.textContent = '';

  if (curSort === SORT_ABILITIES) {
    grid.classList.add('grid-ability');
    renderAbilityGrid(grid);
  } else if (curSort === SORT_STORY) {
    grid.classList.add('grid-roster');
    curWorld = CAT.story || { name: 'Starter Pack', logo: '', characters: [], vehicles: [] };
    renderRosterInto(grid, curWorld);
  } else if (curSort === SORT_FAVORITES) {
    grid.classList.add('grid-roster');
    await renderFavoritesInto(grid);
  } else {
    if (curSort === SORT_USER && !userOrder.length) await refreshSortData();
    renderWorldGrid(grid);
  }
  // Updated last: refreshSortData() above may have changed curSort.
  updateSortBar();
  updateAbilityFilterBar();
  wireScroller(grid, $('franchiseScroll'));
  applySelectionHighlight();
}

function renderWorldGrid(grid) {
  // The favorites and Special tiles lead the Default and User grids (the
  // desktop shows them on its custom-order sort too); the year waves are
  // pure world lists.
  if (curSort === SORT_DEFAULT || curSort === SORT_USER) {
    grid.appendChild(makeFavoriteTile());
    if (CAT.custom && (CAT.custom.characters || []).length) grid.appendChild(makeCustomTile());
  }
  let worlds = CAT.franchises.map((world, idx) => ({ world, idx }));
  if (curSort === SORT_YEAR1) {
    worlds = worlds.filter((x) => x.world.year1);
  } else if (curSort === SORT_YEAR2) {
    worlds = worlds.filter((x) => x.world.year2);
  } else if (curSort === SORT_USER && userOrder.length) {
    worlds = userOrder.map((idx) => ({ world: CAT.franchises[idx], idx })).filter((x) => x.world);
  }
  if (!worlds.length) {
    grid.appendChild(makeEmptyNote('No worlds in this sort.'));
    return;
  }
  worlds.forEach(({ world, idx }) => grid.appendChild(makeWorldTile(world, idx)));
}

function makeWorldTile(world, idx) {
  const tile = document.createElement('div');
  tile.className = 'fworld';
  tile.dataset.world = idx;
  const logo = document.createElement('img');
  logo.className = 'logo';
  logo.src = world.logo;
  logo.alt = world.name;
  tile.appendChild(logo);
  tile.addEventListener('click', () => {
    pushSelection(1, idx);
    onWorldTap(idx, tile);
  });
  return tile;
}

function makeFavoriteTile() {
  const tile = document.createElement('div');
  tile.className = 'fworld';
  tile.dataset.virtual = 'favorites';
  const logo = document.createElement('img');
  logo.className = 'logo';
  logo.src = CAT.favoritesIcon;
  logo.alt = 'Favorites';
  tile.appendChild(logo);
  tile.addEventListener('click', () => {
    pushSelection(2, 0);
    onFavoritesTap(tile);
  });
  return tile;
}

function makeCustomTile() {
  const tile = document.createElement('div');
  tile.className = 'fworld';
  tile.dataset.virtual = 'custom';
  const logo = document.createElement('img');
  logo.className = 'logo';
  logo.src = CAT.customTile || CAT.custom.logo;
  logo.alt = 'Special';
  tile.appendChild(logo);
  tile.addEventListener('click', () => {
    pushSelection(2, 1);
    openCustomRoster();
  });
  return tile;
}

function openCustomRoster() {
  if (!CAT.custom) return;
  curWorld = CAT.custom;
  setWorldLogo(CAT.custom.logo);
  buildRoster(curWorld);
  setScreen('roster');
}

async function renderFavoritesInto(grid) {
  let world;
  try {
    world = await api('/api/favorites');
  } catch (e) {
    grid.appendChild(makeEmptyNote('Could not load favorites.'));
    return;
  }
  curWorld = world;
  renderRosterInto(grid, world);
}

function renderAbilityGrid(grid) {
  const abilities = (CAT.abilities || []).filter((a) => {
    if (abilityFilter === 0) return true;
    return a.section === (CAT.abilitySections || [])[abilityFilter - 1];
  });
  if (!abilities.length) {
    grid.appendChild(makeEmptyNote('No abilities in this filter.'));
    return;
  }
  abilities.forEach((a) => grid.appendChild(makeAbilityTile(a)));
}

function makeAbilityTile(ability) {
  const tile = document.createElement('div');
  tile.className = 'ability';
  tile.dataset.ability = ability.index;
  const panel = document.createElement('div');
  panel.className = 'abpanel';
  if (CAT.abilitiesTile) panel.style.backgroundImage = `url(${CAT.abilitiesTile})`;

  const hasIcon = hasArt(ability.icon);
  if (hasIcon) {
    const img = document.createElement('img');
    img.className = 'abicon';
    img.alt = ability.name;
    img.src = ability.icon;
    panel.appendChild(img);
  } else {
    const letter = document.createElement('span');
    letter.className = 'abletter';
    letter.style.setProperty('--fig-color', ability.color || '#60F3DF');
    letter.textContent = (ability.name || '?').charAt(0);
    panel.appendChild(letter);
  }
  tile.appendChild(panel);

  const lbl = document.createElement('div');
  lbl.className = 'ablbl';
  lbl.textContent = ability.name;
  tile.appendChild(lbl);

  tile.addEventListener('click', () => {
    highlightTouched(tile);
    pushSelection(4, ability.index);
    openAbilityRoster(ability);
  });
  return tile;
}

// Every character / vehicle build that carries this ability, assembled into
// a synthetic "world" so the normal roster renderer can show it - exactly
// what OpenAbilityRoster does on the desktop.
function openAbilityRoster(ability) {
  const idx = ability.index;
  const characters = [];
  const vehicles = [];
  CAT.franchises.forEach((world) => {
    (world.characters || []).forEach((c) => {
      if ((c.abilities || []).includes(idx)) characters.push(c);
    });
    (world.vehicles || []).forEach((group) => {
      const builds = (group.builds || []).filter((b) => (b.abilities || []).includes(idx));
      if (builds.length) vehicles.push({ base: group.base, franchise: group.franchise, builds });
    });
  });
  curWorld = {
    name: ability.name,
    logo: hasArt(ability.icon) ? ability.icon : '',
    characters,
    vehicles,
  };
  setWorldLogo(curWorld.logo);
  buildRoster(curWorld);
  setScreen('roster');
}

function makeEmptyNote(text) {
  const el = document.createElement('div');
  el.className = 'gridnote';
  el.textContent = text;
  return el;
}

function setWorldLogo(url) {
  const el = $('worldLogo');
  if (!el) return;
  const has = hasArt(url);
  el.style.display = has ? '' : 'none';
  if (has) el.src = url;
}

// --- desktop cursor sync --------------------------------------------------
// The desktop's highlighted tile is mirrored here (poll) and a tap on the
// phone pushes its tile back to the desktop (push), so both cursors stay in
// step. Selection kinds match ApplySelectionSet in main.cpp:
//   1 world (franchise index), 2 virtual (0 favorites / 1 custom),
//   3 roster tag id, 4 ability index.
function applySelectionHighlight() {
  const sel = selection || {};
  document.querySelectorAll('.fworld.sel, .fig.sel, .ability.sel')
    .forEach((el) => el.classList.remove('sel'));
  if (sel.world >= 0) {
    const el = document.querySelector(`.fworld[data-world="${sel.world}"]`);
    if (el) el.classList.add('sel');
  } else if (sel.virtual) {
    const el = document.querySelector(`.fworld[data-virtual="${sel.virtual}"]`);
    if (el) el.classList.add('sel');
  }
  if (sel.bin) {
    const el = document.querySelector(`.fig[data-bin="${sel.bin}"]`);
    if (el) el.classList.add('sel');
  }
  if (sel.ability >= 0) {
    const el = document.querySelector(`.ability[data-ability="${sel.ability}"]`);
    if (el) el.classList.add('sel');
  }
}

function pushSelection(kind, value) {
  if (kind === 1) selection = { ...selection, world: value, virtual: '' };
  else if (kind === 2) selection = { ...selection, world: -1, virtual: value ? 'custom' : 'favorites' };
  else if (kind === 3) selection = { ...selection, bin: value };
  else if (kind === 4) selection = { ...selection, ability: value };
  applySelectionHighlight();
  fetch('/api/selection', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ kind, value }),
  }).catch(() => {});
}

async function pollSelection() {
  // Fast while a browsable screen is up (the cursor moves a lot), slow
  // otherwise, so an idle phone doesn't hammer the desktop.
  const browsing = screen === 'franchise' || screen === 'roster';
  try {
    const sel = await api('/api/selection');
    if (sel && sel.ok) {
      selection = sel;
      if (browsing) applySelectionHighlight();
    }
  } catch (e) { /* keep the last highlight */ }
  selectionTimer = setTimeout(pollSelection, browsing ? 350 : 1500);
}

function startSelectionPolling() {
  if (!selectionTimer) pollSelection();
}

// --- LED mirror toggle ----------------------------------------------------
function updateLedToggle() {
  const btn = $('ledToggle');
  if (!btn) return;
  btn.classList.toggle('on', !!(lastState && lastState.ledMirror));
}

async function onLedToggle() {
  const next = !(lastState && lastState.ledMirror);
  try {
    const res = await fetch('/api/ledmirror', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ enabled: next ? 1 : 0 }),
    });
    const data = await res.json();
    if (!data.ok) throw new Error('failed');
    if (lastState) lastState.ledMirror = data.enabled;
    updateLedToggle();
    setStatusMessage(data.enabled ? 'Toypad LED mirror on' : 'Toypad LED mirror off', 'success');
  } catch (e) {
    setStatusMessage('Could not change the LED mirror.', 'error');
  }
}

// --- roster grid ----------------------------------------------------------
function buildRoster(world) {
  const grid = $('rosterGrid');
  grid.textContent = '';
  renderRosterInto(grid, world);
  wireScroller(grid, $('rosterScroll'));
  applySelectionHighlight();
}

// Shared by the roster screen and the browse screen's Story / Favorites
// pages. Characters and vehicles flow together in one continuous grid - no
// separator row (the desktop breaks the two sections apart; the web doesn't).
// Only the default (build 1) tile is shown per vehicle, same as the desktop
// overlay; its alternates are revealed through the build picker when the tile
// itself is pressed, so there's no separate "+" grid slot.
function renderRosterInto(grid, world) {
  const chars = (world && world.characters) || [];
  const vehs = (world && world.vehicles) || [];
  if (!chars.length && !vehs.length) {
    grid.appendChild(makeEmptyNote('Nothing here yet.'));
    return;
  }
  chars.forEach((e) => grid.appendChild(makeFig(e)));
  vehs.forEach((group) => {
    const entry = group.builds[0];
    grid.appendChild(makeFig(entry, group.builds.length > 1 ? group : null));
  });
}

function makeFig(entry, group) {
  const fig = document.createElement('div');
  fig.className = 'fig';
  fig.dataset.bin = entry.bin;
  const ring = document.createElement('div');
  ring.className = 'ring bordered';
  ring.style.setProperty('--fig-color', entry.color);
  const hasPortrait = hasArt(entry.portrait);
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
    pushSelection(3, entry.bin);
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
// Wired once per scroller element; the grid contents are rebuilt on every
// sort change, so re-adding a listener each time would leak handlers.
function wireScroller(scroller, sb) {
  sb.querySelector('img').src = CAT.scrollBar;
  scroller._scrollbar = sb;
  if (!scroller._scrollerWired) {
    scroller._scrollerWired = true;
    scroller.addEventListener('scroll', () => drawThumb(scroller, scroller._scrollbar));
  }
  updateScroller(scroller);
}

function updateScroller(scroller) {
  const sb = scroller && scroller._scrollbar;
  if (!sb) return;
  const can = scroller.scrollHeight > scroller.clientHeight + 1;
  sb.classList.toggle('visible', can);
  drawThumb(scroller, sb);
}

function updateAllScrollers() {
  updateScroller($('franchiseGrid'));
  updateScroller($('rosterGrid'));
}

function drawThumb(scroller, sb) {
  const img = sb.querySelector('img');
  const ratio = scroller.clientHeight / Math.max(1, scroller.scrollHeight);
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
  closeAbilities();
  $('padScreen').classList.toggle('active', s === 'pad');
  $('franchiseScreen').classList.toggle('active', s === 'franchise');
  $('rosterScreen').classList.toggle('active', s === 'roster');
  $('plusScreen').classList.toggle('active', s === 'plus');
  $('backBtn').classList.toggle('visible', s !== 'pad');
  $('floatName').classList.remove('show');
  if (s === 'pad') {
    squareCenterPad();
    updateFloatName();
  }
  if (s === 'franchise') renderBrowse();
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

// Finds a catalog entry (character or vehicle build) by its tag id, so the
// pad's loaded figure can be resolved back to its abilities[] list.
function findEntryByBin(bin) {
  for (const world of CAT.franchises || []) {
    for (const c of world.characters || []) if (c.bin === bin) return c;
    for (const g of world.vehicles || []) {
      for (const b of g.builds || []) if (b.bin === bin) return b;
    }
  }
  if (CAT.custom) {
    for (const c of CAT.custom.characters || []) if (c.bin === bin) return c;
  }
  return null;
}

function abilityByIndex(index) {
  return (CAT.abilities || []).find((a) => a.index === index) || null;
}

// ABILITIES button: show the abilities of whatever is loaded on the selected
// pad, the web counterpart to the desktop's abilities peek.
function onAbilitiesTap(btn) {
  tapAnimate(btn);
  if (curSlot === null) {
    setStatusMessage('Tap a pad first to select a slot.', 'warn');
    return;
  }
  const pad = lastState && lastState.pads ? lastState.pads[curSlot] : null;
  if (!pad || !pad.occupied) {
    setStatusMessage('Nothing on this pad to show abilities for.', 'warn');
    return;
  }
  const entry = findEntryByBin(pad.bin);
  const indices = (entry && entry.abilities) || [];
  if (!indices.length) {
    setStatusMessage(`"${pad.name}" has no abilities.`, 'warn');
    return;
  }
  openAbilities(pad.name, indices);
}

function openAbilities(name, indices) {
  $('abilitiesName').textContent = name;
  const list = $('abilitiesList');
  list.textContent = '';
  indices.forEach((index) => {
    const ability = abilityByIndex(index);
    if (!ability) return;
    const item = document.createElement('div');
    item.className = 'abilityItem';
    const hasIcon = hasArt(ability.icon);
    if (hasIcon) {
      const img = document.createElement('img');
      img.src = ability.icon;
      img.alt = ability.name;
      item.appendChild(img);
    } else {
      const ph = document.createElement('span');
      ph.className = 'abilityPh';
      ph.style.setProperty('--fig-color', ability.color || '#A878FF');
      ph.textContent = (ability.name || '?').charAt(0);
      item.appendChild(ph);
    }
    const lbl = document.createElement('div');
    lbl.className = 'abilityItemLbl';
    lbl.textContent = ability.name;
    item.appendChild(lbl);
    list.appendChild(item);
  });
  $('abilitiesOverlay').classList.add('show');
}

function closeAbilities() {
  const overlay = $('abilitiesOverlay');
  if (overlay) overlay.classList.remove('show');
}

function onWorldTap(idx, tile) {
  curWorld = CAT.franchises[idx];
  highlightTouched(tile);
  setWorldLogo(curWorld.logo);
  buildRoster(curWorld);
  setScreen('roster');
}

// The favorites tile now switches the browse screen to the Favorites sort,
// whose page renders the same roster inline (still synced with the desktop).
function onFavoritesTap(tile) {
  highlightTouched(tile);
  changeSort(SORT_FAVORITES);
}

function openPlus(group) {
  plusReturn = screen === 'plus' ? plusReturn : screen;
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
    updateLedToggle();
    if (s && s.background && s.background !== currentBg) {
      currentBg = s.background;
      const bg = $('bgimg');
      if (bg) bg.src = s.background;
    }
    // Keep the sort page in step with the desktop, which can change it from
    // its own shoulder buttons while this page is open.
    if (s && typeof s.sort === 'number' && s.sort !== curSort) {
      curSort = s.sort;
      if (screen === 'franchise') renderBrowse();
      else updateSortBar();
    }
    return s;
  } catch (e) {
    return null;
  }
}

// --- toypad LED mirror -----------------------------------------------------
// Polled on its own faster loop (not the 3s /api/state one): Flash and Fade
// need to read as motion, not a slideshow. Backs off to a slow idle check
// while the desktop app's "Toypad LEDs" setting is off.
let ledPollTimer = null;

function applyLedState(state) {
  const regions = (state && state.regions) || [];
  for (let region = 0; region < LED_REGION_SLOTS.length; region++) {
    const r = regions[region];
    const on = state.enabled && r && r.mode !== 'off' && r.intensity > 0.004;
    const color = on ? r.color : 'transparent';
    const intensity = on ? r.intensity : 0;
    for (const slot of LED_REGION_SLOTS[region]) {
      const pad = pads[slot];
      if (!pad) continue;
      pad.style.setProperty('--led-color', color);
      pad.style.setProperty('--led-intensity', String(intensity));
    }
  }
}

function clearLedState() {
  for (const pad of pads) {
    pad.style.setProperty('--led-color', 'transparent');
    pad.style.setProperty('--led-intensity', '0');
  }
}

async function pollLeds() {
  let next = 2000; // idle check cadence while LEDs are off / unreachable
  try {
    const state = await api('/api/leds');
    applyLedState(state);
    if (state.enabled) next = 90; // smooth enough for Flash/Fade
  } catch (e) {
    clearLedState();
  }
  ledPollTimer = setTimeout(pollLeds, next);
}

function startLedPolling() {
  if (ledPollTimer) return;
  pollLeds();
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
    setScreen(plusReturn);
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
  window.addEventListener('resize', squareCenterPad);
  window.addEventListener('orientationchange', () => setTimeout(() => {
    repositionPads();
    updateFloatName();
  }, 200));
  window.addEventListener('resize', updateAllScrollers);
  window.addEventListener('orientationchange', () => setTimeout(updateAllScrollers, 300));
  $('backBtn').addEventListener('click', goBack);
  $('sortPrev').addEventListener('click', () => cycleSort(-1));
  $('sortNext').addEventListener('click', () => cycleSort(+1));
  $('ledToggle').addEventListener('click', onLedToggle);
  $('abilitiesClose').addEventListener('click', closeAbilities);
  $('abilitiesOverlay').addEventListener('click', (e) => {
    if (e.target === $('abilitiesOverlay')) closeAbilities();
  });
  window.addEventListener('keydown', (e) => {
    if (e.key === 'Escape') closeAbilities();
  });

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
  buildAbilityFilterBar();

  setScreen('pad');
  await getState();
  startLedPolling();
  startSelectionPolling();
  setStatusMessage('Tap a pad to select, double-tap to browse characters');
  setTimeout(() => setLoading(false), 350);
  setInterval(getState, 3000);
}

boot();
