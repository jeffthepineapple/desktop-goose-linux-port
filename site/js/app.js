import { drawStage, drawIcon, drawNoneIcon, STYLE_META } from './render.js';

const $ = (selector, root = document) => root.querySelector(selector);
const $$ = (selector, root = document) => [...root.querySelectorAll(selector)];
const reducedMotion = matchMedia('(prefers-reduced-motion: reduce)').matches;

// ---- Data lifted from src/cosmetics.cpp -------------------------------------

const BUILT_IN_ITEMS = {
  'red-cap': { style: 'baseball-cap', primary: '#d11f1a', accent: '#faad29' },
  'blue-cap': { style: 'baseball-cap', primary: '#1a59b8', accent: '#59c7f2' },
  'top-hat': { style: 'top-hat', primary: '#14171f', accent: '#d12e2e' },
  'party-hat': { style: 'party-hat', primary: '#1fb8c7', accent: '#fa8c1a' },
  'crown': { style: 'crown', primary: '#f5ad14', accent: '#cc261f' },
  'round': { style: 'round', primary: '#1a1a1f', accent: '#b8e0f5' },
  'shades': { style: 'shades', primary: '#0a0d12', accent: '#47add1' },
  'aviators': { style: 'aviators', primary: '#c7851a', accent: '#42addb' },
};

const BUILT_IN_LOOKS = [
  { id: 'classic', label: 'Classic goose', hat: null, glasses: null },
  { id: 'scholar', label: 'The Scholar', hat: 'top-hat', glasses: 'round' },
  { id: 'party', label: 'Party Bird', hat: 'party-hat', glasses: 'shades' },
  { id: 'pilot', label: 'The Pilot', hat: 'blue-cap', glasses: 'aviators' },
  { id: 'royal', label: 'Royal Goose', hat: 'crown', glasses: 'round' },
  { id: 'incognito', label: 'Incognito', hat: 'red-cap', glasses: 'shades' },
];
const RESERVED = new Set(BUILT_IN_LOOKS.map((look) => look.id));

const PALETTE = [
  ['Ink', '#141417'], ['White', '#ffffff'], ['Red', '#d11f1a'], ['Orange', '#fa8c1a'],
  ['Gold', '#f5ad14'], ['Lime', '#5aa80f'], ['Teal', '#1fb8c7'], ['Sky', '#42addb'],
  ['Blue', '#1a59b8'], ['Purple', '#7b3fc4'], ['Pink', '#e0408f'], ['Mud', '#73401a'],
];

const SLOTS = ['hat', 'glasses'];
const ROLES = [['primary', 'Main'], ['accent', 'Trim']];
const POSES = [90, 0, -55, -90];
const HEX = /^#[0-9a-f]{6}$/i;
const NAME_RE = /^[a-z0-9_-]{1,24}$/;
const GOOSE_RE = /^[A-Za-z0-9_-]{1,32}$/;
const STORE_STATE = 'gsc.state.v1';
const STORE_WARDROBE = 'gsc.wardrobe.v1';

// ---- State ------------------------------------------------------------------

const defaults = () => ({
  name: 'my-goose',
  dir: 40,
  wall: 'lawn',
  hat: { style: 'party-hat', primary: '#7b3fc4', accent: '#ffa300' },
  glasses: { style: 'round', primary: '#141417', accent: '#42addb' },
});

let state = defaults();
let wardrobe = [];
let exportTab = 'cmd';

const cloneSlot = (slot) => ({ style: slot.style, primary: slot.primary, accent: slot.accent });
const skinOf = (s) => ({
  hat: s.hat.style ? { ...s.hat } : null,
  glasses: s.glasses.style ? { ...s.glasses } : null,
});

function sanitizeName(raw) {
  return raw.toLowerCase().replace(/\s+/g, '-').replace(/[^a-z0-9_-]/g, '').slice(0, 24);
}

function validSlot(raw, slot, fallback) {
  if (!raw || typeof raw !== 'object') return fallback;
  const meta = STYLE_META[raw.style];
  const style = meta && meta.slot === slot ? raw.style : raw.style === null ? null : fallback.style;
  return {
    style,
    primary: HEX.test(raw.primary) ? raw.primary.toLowerCase() : fallback.primary,
    accent: HEX.test(raw.accent) ? raw.accent.toLowerCase() : fallback.accent,
  };
}

function normalizeState(raw) {
  const base = defaults();
  if (!raw || typeof raw !== 'object') return base;
  return {
    name: typeof raw.name === 'string' ? sanitizeName(raw.name) : base.name,
    dir: Number.isFinite(raw.dir) ? Math.max(-180, Math.min(180, raw.dir)) : base.dir,
    wall: ['lawn', 'night', 'paper', 'clear'].includes(raw.wall) ? raw.wall : base.wall,
    hat: validSlot(raw.hat, 'hat', base.hat),
    glasses: validSlot(raw.glasses, 'glasses', base.glasses),
  };
}

function readHash() {
  const params = new URLSearchParams(location.hash.slice(1));
  if (!params.has('n')) return null;
  const slot = (value, name, fallback) => {
    if (value === null) return fallback;
    if (value === 'none') return { ...fallback, style: null };
    const [style, primary, accent] = value.split(',');
    return validSlot({ style, primary: `#${primary}`, accent: `#${accent}` }, name, fallback);
  };
  const base = defaults();
  return normalizeState({
    name: params.get('n'),
    dir: Number(params.get('d') ?? base.dir),
    hat: slot(params.get('h'), 'hat', base.hat),
    glasses: slot(params.get('g'), 'glasses', base.glasses),
  });
}

function hashFor(s) {
  const slot = (v) => (v.style ? `${v.style},${v.primary.slice(1)},${v.accent.slice(1)}` : 'none');
  const params = new URLSearchParams({ n: s.name, h: slot(s.hat), g: slot(s.glasses), d: String(Math.round(s.dir)) });
  return `#${params.toString()}`;
}

function load() {
  const fromHash = readHash();
  if (fromHash) {
    state = fromHash;
  } else {
    try { state = normalizeState(JSON.parse(localStorage.getItem(STORE_STATE))); } catch { state = defaults(); }
  }
  try {
    const raw = JSON.parse(localStorage.getItem(STORE_WARDROBE));
    wardrobe = Array.isArray(raw)
      ? raw.map(normalizeState).filter((look) => NAME_RE.test(look.name) && !RESERVED.has(look.name)).slice(0, 24)
      : [];
  } catch { wardrobe = []; }
}

function persist() {
  try {
    localStorage.setItem(STORE_STATE, JSON.stringify(state));
    localStorage.setItem(STORE_WARDROBE, JSON.stringify(wardrobe));
  } catch { /* storage unavailable: the page still works */ }
}

// ---- Animation --------------------------------------------------------------

const anim = { pop: null, honk: null, tween: null };
let frame = 0;
let dirty = true;

function requestRender() {
  dirty = true;
  if (!frame) frame = requestAnimationFrame(tick);
}

function popCosmetics() {
  if (!reducedMotion) anim.pop = performance.now();
  requestRender();
}

function turnTo(target) {
  if (reducedMotion) {
    state.dir = target;
  } else {
    let delta = ((target - state.dir + 540) % 360) - 180;
    if (Math.abs(delta) < 0.5) delta = 0;
    anim.tween = { from: state.dir, delta, start: performance.now(), duration: 380 };
  }
  requestRender();
}

function honkAnimation(now) {
  const t = (now - anim.honk) / 1000;
  if (t > 0.75) { anim.honk = null; return 0; }
  const rise = Math.min(t / 0.12, 1);
  const fall = t < 0.35 ? 1 : 1 - (t - 0.35) / 0.4;
  const eased = 1 - Math.pow(1 - rise, 3);
  return Math.max(0, Math.min(1, eased * fall));
}

function tick(now) {
  frame = 0;
  let animating = false;

  if (anim.tween) {
    const progress = Math.min((now - anim.tween.start) / anim.tween.duration, 1);
    const eased = 1 - Math.pow(1 - progress, 4);
    state.dir = normalizeDir(anim.tween.from + anim.tween.delta * eased);
    if (progress >= 1) {
      anim.tween = null;
      history.replaceState(null, '', hashFor(state));
      persist();
    } else {
      animating = true;
    }
    dirty = true;
    syncDirControls();
  }

  let pop = 1;
  if (anim.pop !== null) {
    const t = (now - anim.pop) / 1000;
    if (t > 0.9) anim.pop = null;
    else { pop = 1 + 0.32 * Math.exp(-6 * t) * Math.cos(17 * t); animating = true; }
    dirty = true;
  }

  let neckLerp = 0;
  if (anim.honk !== null) {
    neckLerp = honkAnimation(now);
    animating = anim.honk !== null;
    dirty = true;
  }

  if (dirty) {
    dirty = false;
    drawStage(els.stage, { dir: state.dir, neckLerp, pop, skin: skinOf(state) });
  }
  if (animating) frame = requestAnimationFrame(tick);
}

function normalizeDir(deg) {
  return ((deg + 540) % 360) - 180;
}

// ---- Sound ------------------------------------------------------------------

let audio = null;
function honkSound() {
  const Context = window.AudioContext || window.webkitAudioContext;
  if (!Context) return;
  audio = audio || new Context();
  if (audio.state === 'suspended') audio.resume();
  const t = audio.currentTime;
  const gain = audio.createGain();
  gain.gain.setValueAtTime(0.0001, t);
  gain.gain.exponentialRampToValueAtTime(0.45, t + 0.03);
  gain.gain.setValueAtTime(0.45, t + 0.2);
  gain.gain.exponentialRampToValueAtTime(0.0001, t + 0.5);
  const band = audio.createBiquadFilter();
  band.type = 'bandpass';
  band.frequency.value = 950;
  band.Q.value = 1.8;
  for (const ratio of [1, 1.5]) {
    const osc = audio.createOscillator();
    osc.type = 'sawtooth';
    osc.frequency.setValueAtTime(250 * ratio, t);
    osc.frequency.exponentialRampToValueAtTime(350 * ratio, t + 0.09);
    osc.frequency.exponentialRampToValueAtTime(230 * ratio, t + 0.48);
    osc.connect(band);
    osc.start(t);
    osc.stop(t + 0.52);
  }
  band.connect(gain).connect(audio.destination);
}

// ---- Controls ---------------------------------------------------------------

const els = {};

function buildControls() {
  for (const slot of SLOTS) {
    const tiles = $(`.tiles[data-slot="${slot}"]`);
    const styles = Object.entries(STYLE_META).filter(([, meta]) => meta.slot === slot);
    const options = [[null, 'None'], ...styles.map(([style, meta]) => [style, meta.label])];
    tiles.innerHTML = options.map(([style, label]) => `
      <label class="tile">
        <input type="radio" name="style-${slot}" value="${style ?? 'none'}">
        <span class="tile-face"><canvas width="52" height="52" data-style="${style ?? 'none'}" aria-hidden="true"></canvas><span>${label}</span></span>
      </label>`).join('');

    $(`.colors[data-slot="${slot}"]`).innerHTML = ROLES.map(([role, label]) => `
      <div class="colorrow" data-role="${role}">
        <div class="colorhead">
          <span class="nametag">${label}</span>
          <span class="custom">
            <input type="color" class="picker" aria-label="${slot} ${label.toLowerCase()} color, custom">
            <input type="text" class="hex" maxlength="7" spellcheck="false" autocomplete="off" aria-label="${slot} ${label.toLowerCase()} color as hex">
          </span>
        </div>
        <div class="swatches">
          ${PALETTE.map(([name, hex]) => `<button type="button" class="swatch" style="background:${hex}" data-hex="${hex}" aria-label="${name}" aria-pressed="false"></button>`).join('')}
        </div>
      </div>`).join('');
  }

  $('#presets').innerHTML = BUILT_IN_LOOKS.map((look) =>
    `<button type="button" class="chip" data-look="${look.id}">${look.label}</button>`).join('');
}

function syncControls() {
  $('#name').value = state.name;
  $('#nametag').textContent = state.name || 'unnamed';
  $('#turn').value = Math.round(state.dir);
  for (const slot of SLOTS) {
    const current = state[slot].style ?? 'none';
    for (const radio of $$(`input[name="style-${slot}"]`)) radio.checked = radio.value === current;
    $(`.colors[data-slot="${slot}"]`).dataset.off = String(!state[slot].style);
    for (const [role] of ROLES) {
      const row = $(`.colors[data-slot="${slot}"] .colorrow[data-role="${role}"]`);
      const value = state[slot][role];
      for (const swatch of $$('.swatch', row)) swatch.setAttribute('aria-pressed', String(swatch.dataset.hex === value));
      $('.picker', row).value = value;
      const hex = $('.hex', row);
      if (document.activeElement !== hex) { hex.value = value; hex.removeAttribute('aria-invalid'); }
    }
  }
  syncDirControls();
  for (const chip of $$('.chip[data-wall]')) chip.setAttribute('aria-pressed', String(chip.dataset.wall === state.wall));
  $('#screen').dataset.wall = state.wall;
}

function syncDirControls() {
  $('#turn').value = Math.round(state.dir);
  for (const pose of $$('.pose')) {
    const target = Number(pose.dataset.dir);
    const diff = Math.abs(normalizeDir(state.dir - target));
    pose.setAttribute('aria-pressed', String(diff < 3));
  }
}

function drawPickers() {
  for (const slot of SLOTS) {
    for (const canvas of $$(`.tiles[data-slot="${slot}"] canvas`)) {
      if (canvas.dataset.style === 'none') { drawNoneIcon(canvas); continue; }
      const source = state[slot];
      drawIcon(canvas, { style: canvas.dataset.style, primary: source.primary, accent: source.accent });
    }
  }
}

function drawPoses() {
  const skin = skinOf(state);
  $$('.pose').forEach((button) => {
    drawStage($('canvas', button), { dir: Number(button.dataset.dir), neckLerp: 0, pop: 1, skin }, 0.98);
  });
}

function isPale(hex) {
  const n = parseInt(hex.slice(1), 16);
  const [r, g, b] = [(n >> 16) & 255, (n >> 8) & 255, n & 255].map((v) => v / 255);
  const max = Math.max(r, g, b);
  const min = Math.min(r, g, b);
  const l = (max + min) / 2;
  const s = max === min ? 0 : (max - min) / (1 - Math.abs(2 * l - 1));
  if (l > 0.93 || l < 0.2) return false;
  return (l > 0.72 && s > 0.2) || (s < 0.2 && l > 0.3 && l < 0.88);
}

function updateWarning() {
  const pale = [];
  for (const slot of SLOTS) {
    if (!state[slot].style) continue;
    for (const [role, label] of ROLES) {
      if (isPale(state[slot][role])) pale.push(`${slot} ${label.toLowerCase()}`);
    }
  }
  const soft = $('#soft');
  soft.hidden = pale.length === 0;
  soft.textContent = pale.length
    ? `Pale or gray colors (${pale.join(', ')}) can disappear on light wallpapers. The built-in set sticks to saturated colors.`
    : '';
}

// ---- Export -----------------------------------------------------------------

function nameProblem() {
  if (!state.name) return 'Give the look a name.';
  if (!NAME_RE.test(state.name)) return 'Names use 1 to 24 lowercase letters, numbers, - or _.';
  if (RESERVED.has(state.name)) return `"${state.name}" is a built-in look, so the goose ignores a custom one with that name. Pick another name.`;
  return '';
}

function iniFor(looks) {
  const lines = ['# Made with Goose Skin Creator'];
  for (const look of looks) {
    for (const slot of SLOTS) {
      const item = look[slot];
      if (!item.style) continue;
      const id = `${look.name}_${slot}`;
      lines.push(`item.${id}.label=${look.name} ${slot}`);
      lines.push(`item.${id}.style=${item.style}`);
      lines.push(`item.${id}.primary=${item.primary}`);
      lines.push(`item.${id}.accent=${item.accent}`);
    }
    for (const slot of SLOTS) {
      lines.push(`profile.${look.name}.${slot}=${look[slot].style ? `${look.name}_${slot}` : 'none'}`);
    }
  }
  return lines.join('\n');
}

function exportLooks() {
  const saved = $('#all').checked ? wardrobe.filter((look) => look.name !== state.name) : [];
  return [...saved, state];
}

function exportTexts() {
  const ini = iniFor(exportLooks());
  const gooseId = $('#gooseid').value.trim();
  const command = [
    'cfg="${XDG_CONFIG_HOME:-$HOME/.config}/desktop-goose"',
    'mkdir -p "$cfg"',
    `cat >> "$cfg/skins.ini" <<'EOF'`,
    ini,
    'EOF',
    `CppGoose skins reload && CppGoose skins equip ${gooseId} ${state.name}`,
  ].join('\n');
  return { ini, command };
}

const escapeHtml = (text) => text.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');

function highlight(text) {
  return escapeHtml(text).split('\n').map((line) => {
    if (line.startsWith('#')) return `<span class="c">${line}</span>`;
    return line.replace(/^((?:item|profile)\.[^=]+)=/, '<span class="k">$1</span>=');
  }).join('\n');
}

function updateExport() {
  const problem = nameProblem();
  const gooseOk = GOOSE_RE.test($('#gooseid').value.trim());
  const probEl = $('#name-problem');
  probEl.hidden = !problem;
  probEl.textContent = problem;
  $('#name').toggleAttribute('aria-invalid', !!problem);
  $('#gooseid').toggleAttribute('aria-invalid', !gooseOk);

  const ready = !problem && gooseOk;
  for (const id of ['copy', 'download']) $(`#${id}`).disabled = !ready;

  const code = $('#code');
  if (!ready) {
    code.textContent = problem || 'Goose ids are letters, numbers, - or _. Run CppGoose status to see yours.';
  } else {
    const texts = exportTexts();
    code.innerHTML = highlight(exportTab === 'cmd' ? texts.command : texts.ini);
  }
  $('#ini-note').hidden = exportTab !== 'ini';
  $('#copy').textContent = exportTab === 'cmd' ? 'Copy command' : 'Copy skins.ini';
}

function setTab(tab) {
  exportTab = tab;
  for (const [id, key] of [['tab-cmd', 'cmd'], ['tab-ini', 'ini']]) {
    const button = $(`#${id}`);
    button.setAttribute('aria-selected', String(key === tab));
    button.tabIndex = key === tab ? 0 : -1;
  }
  $('#panel-code').setAttribute('aria-labelledby', tab === 'cmd' ? 'tab-cmd' : 'tab-ini');
  updateExport();
}

let toastTimer = 0;
function toast(message) {
  const el = $('#toast');
  el.textContent = message;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { el.textContent = ''; }, 3200);
}

async function copyText(text, done) {
  try {
    await navigator.clipboard.writeText(text);
    toast(done);
  } catch {
    const area = document.createElement('textarea');
    area.value = text;
    area.style.position = 'fixed';
    area.style.opacity = '0';
    document.body.append(area);
    area.select();
    const ok = document.execCommand('copy');
    area.remove();
    toast(ok ? done : 'Copy failed. Select the text and copy it by hand.');
  }
}

function download(blob, filename) {
  const url = URL.createObjectURL(blob);
  const link = document.createElement('a');
  link.href = url;
  link.download = filename;
  document.body.append(link);
  link.click();
  link.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

// ---- Wardrobe ---------------------------------------------------------------

function renderWardrobe() {
  const list = $('#wardrobe');
  list.innerHTML = wardrobe.map((look, index) => `
    <li class="look">
      <button type="button" class="look-open" data-index="${index}" aria-label="Load ${look.name}">
        <canvas aria-hidden="true"></canvas><span class="nametag">${look.name}</span>
      </button>
      <button type="button" class="look-remove" data-index="${index}" aria-label="Remove ${look.name}">
        <svg viewBox="0 0 12 12" aria-hidden="true"><path d="M2 2l8 8M10 2l-8 8" stroke="#141417" stroke-width="2.2" stroke-linecap="round"/></svg>
      </button>
    </li>`).join('');
  $('#wardrobe-empty').hidden = wardrobe.length > 0;
  $$('.look', list).forEach((item, index) => {
    const look = wardrobe[index];
    drawStage($('canvas', item), { dir: 30, neckLerp: 0, pop: 1, skin: skinOf(look) }, 0.98);
  });
}

// ---- Wiring -----------------------------------------------------------------

function refresh({ pickers = true, poses = true, pop = false } = {}) {
  syncControls();
  if (pickers) drawPickers();
  if (poses) drawPoses();
  updateWarning();
  updateExport();
  persist();
  history.replaceState(null, '', hashFor(state));
  if (pop) popCosmetics(); else requestRender();
}

function setColor(slot, role, value) {
  state[slot][role] = value.toLowerCase();
  refresh();
}

function wire() {
  $('#name').addEventListener('input', (event) => {
    const clean = sanitizeName(event.target.value);
    state.name = clean;
    if (event.target.value !== clean) event.target.value = clean;
    $('#nametag').textContent = clean || 'unnamed';
    updateExport();
    persist();
    history.replaceState(null, '', hashFor(state));
  });

  $('#presets').addEventListener('click', (event) => {
    const chip = event.target.closest('[data-look]');
    if (!chip) return;
    const look = BUILT_IN_LOOKS.find((entry) => entry.id === chip.dataset.look);
    for (const slot of SLOTS) {
      const item = look[slot] && BUILT_IN_ITEMS[look[slot]];
      state[slot] = item
        ? { style: item.style, primary: item.primary, accent: item.accent }
        : { ...state[slot], style: null };
    }
    refresh({ pop: true });
  });

  for (const slot of SLOTS) {
    $(`.tiles[data-slot="${slot}"]`).addEventListener('change', (event) => {
      const value = event.target.value;
      state[slot].style = value === 'none' ? null : value;
      refresh({ pop: true });
    });
    const colors = $(`.colors[data-slot="${slot}"]`);
    colors.addEventListener('click', (event) => {
      const swatch = event.target.closest('.swatch');
      if (swatch) setColor(slot, swatch.closest('.colorrow').dataset.role, swatch.dataset.hex);
    });
    colors.addEventListener('input', (event) => {
      const row = event.target.closest('.colorrow');
      if (!row) return;
      const role = row.dataset.role;
      if (event.target.classList.contains('picker')) {
        setColor(slot, role, event.target.value);
      } else if (event.target.classList.contains('hex')) {
        const text = event.target.value.trim();
        const hex = text.startsWith('#') ? text : `#${text}`;
        if (HEX.test(hex)) { event.target.removeAttribute('aria-invalid'); setColor(slot, role, hex); }
        else event.target.setAttribute('aria-invalid', 'true');
      }
    });
    colors.addEventListener('focusout', (event) => {
      if (event.target.classList.contains('hex')) {
        event.target.value = state[slot][event.target.closest('.colorrow').dataset.role];
        event.target.removeAttribute('aria-invalid');
      }
    });
  }

  $('#turn').addEventListener('input', (event) => {
    anim.tween = null;
    state.dir = Number(event.target.value);
    syncDirControls();
    history.replaceState(null, '', hashFor(state));
    requestRender();
  });
  for (const pose of $$('.pose')) pose.addEventListener('click', () => turnTo(Number(pose.dataset.dir)));

  for (const chip of $$('.chip[data-wall]')) {
    chip.addEventListener('click', () => {
      state.wall = chip.dataset.wall;
      syncControls();
      persist();
    });
  }

  // Drag the goose around.
  const stage = els.stage;
  let drag = null;
  stage.addEventListener('pointerdown', (event) => {
    anim.tween = null;
    drag = { x: event.clientX, dir: state.dir };
    stage.setPointerCapture(event.pointerId);
    stage.classList.add('dragging');
    $('.drag-hint').classList.add('gone');
  });
  stage.addEventListener('pointermove', (event) => {
    if (!drag) return;
    state.dir = normalizeDir(drag.dir + (event.clientX - drag.x) * 0.7);
    syncDirControls();
    requestRender();
  });
  const endDrag = () => {
    if (!drag) return;
    drag = null;
    stage.classList.remove('dragging');
    history.replaceState(null, '', hashFor(state));
    persist();
  };
  stage.addEventListener('pointerup', endDrag);
  stage.addEventListener('pointercancel', endDrag);
  stage.addEventListener('keydown', (event) => {
    const step = event.key === 'ArrowLeft' ? -8 : event.key === 'ArrowRight' ? 8 : 0;
    if (!step) return;
    event.preventDefault();
    anim.tween = null;
    state.dir = normalizeDir(state.dir + step);
    syncDirControls();
    requestRender();
  });

  $('#honk').addEventListener('click', () => {
    honkSound();
    if (!reducedMotion) anim.honk = performance.now();
    requestRender();
  });

  $('#png').addEventListener('click', () => {
    const canvas = document.createElement('canvas');
    canvas.width = 512;
    canvas.height = 512;
    drawStage(canvas, { dir: state.dir, neckLerp: 0, pop: 1, skin: skinOf(state) }, 0.96, 1);
    canvas.toBlob((blob) => blob && download(blob, `${state.name || 'goose'}.png`), 'image/png');
  });

  $('#save').addEventListener('click', () => {
    const problem = nameProblem();
    if (problem) { toast(problem); $('#name').focus(); return; }
    const copy = normalizeState(state);
    const index = wardrobe.findIndex((look) => look.name === copy.name);
    if (index >= 0) wardrobe[index] = copy; else wardrobe.push(copy);
    wardrobe = wardrobe.slice(-24);
    renderWardrobe();
    updateExport();
    persist();
    toast(index >= 0 ? `Updated ${copy.name} in your wardrobe.` : `Saved ${copy.name} to your wardrobe.`);
  });

  $('#wardrobe').addEventListener('click', (event) => {
    const open = event.target.closest('.look-open');
    const remove = event.target.closest('.look-remove');
    if (open) {
      const look = wardrobe[Number(open.dataset.index)];
      state = { ...normalizeState(look), dir: state.dir, wall: state.wall };
      refresh({ pop: true });
    } else if (remove) {
      const [gone] = wardrobe.splice(Number(remove.dataset.index), 1);
      renderWardrobe();
      updateExport();
      persist();
      toast(`Removed ${gone.name}.`);
    }
  });

  $('#gooseid').addEventListener('input', updateExport);
  $('#all').addEventListener('change', updateExport);

  const tabs = [$('#tab-cmd'), $('#tab-ini')];
  tabs.forEach((tab, index) => {
    tab.addEventListener('click', () => setTab(index === 0 ? 'cmd' : 'ini'));
    tab.addEventListener('keydown', (event) => {
      if (event.key !== 'ArrowLeft' && event.key !== 'ArrowRight') return;
      const next = tabs[(index + 1) % 2];
      next.focus();
      setTab(next === tabs[0] ? 'cmd' : 'ini');
    });
  });

  $('#copy').addEventListener('click', () => {
    const texts = exportTexts();
    copyText(exportTab === 'cmd' ? texts.command : texts.ini, exportTab === 'cmd' ? 'Command copied. Paste it in a terminal.' : 'skins.ini copied.');
  });
  $('#download').addEventListener('click', () => {
    download(new Blob([exportTexts().ini + '\n'], { type: 'text/plain' }), 'skins.ini');
    toast('Downloaded skins.ini.');
  });
  $('#share').addEventListener('click', () => {
    copyText(`${location.origin}${location.pathname}${hashFor(state)}`, 'Link copied. It opens this exact look.');
  });

  new ResizeObserver(() => { drawPoses(); requestRender(); }).observe(els.stage);
  window.addEventListener('resize', () => { drawPickers(); renderWardrobe(); });
}

function start() {
  els.stage = $('#stage');
  load();
  buildControls();
  wire();
  renderWardrobe();
  setTab('cmd');
  refresh({ pop: true });
  document.fonts?.ready.then(() => { drawPickers(); drawPoses(); requestRender(); });
}

start();
