// Brukergrensesnitt: knotter, tastatur, bølgeform, zone-kart og nivåmåler.

import { PARAMS, SECTION_TITLES, valueToNorm, normToValue, formatValue } from './params.js';

const root = getComputedStyle(document.documentElement);
const C = Object.fromEntries(
  ['panel', 'edge', 'track', 'body', 'accent', 'accent2', 'text', 'dim']
    .map((n) => [n, root.getPropertyValue('--' + n).trim()]));

const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));

function el(tag, className, text) {
  const e = document.createElement(tag);
  if (className) e.className = className;
  if (text !== undefined) e.textContent = text;
  return e;
}

// Holder canvasets pikselstørrelse i takt med CSS-størrelsen (og skarpt på HiDPI).
function autoSize(canvas, onResize) {
  const ctx = canvas.getContext('2d');
  const fit = () => {
    const dpr = window.devicePixelRatio || 1;
    const w = Math.max(1, Math.round(canvas.clientWidth * dpr));
    const h = Math.max(1, Math.round(canvas.clientHeight * dpr));
    if (canvas.width !== w || canvas.height !== h) { canvas.width = w; canvas.height = h; }
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    onResize();
  };
  new ResizeObserver(fit).observe(canvas);
  return ctx;
}

// ------------------------------------------------------------------ knotter

const KNOB_START = 0.75 * Math.PI;
const KNOB_END = 2.25 * Math.PI;

// values: objekt med gjeldende verdier (id -> tall). onChange(id, value) kalles kun ved brukerinput.
export function createKnobs(container, values, onChange) {
  const knobs = new Map();
  const sections = [...new Set(PARAMS.map((q) => q.section))];

  for (const section of sections) {
    const list = PARAMS.filter((q) => q.section === section);
    const group = el('div', 'group');
    group.style.setProperty('--n', list.length);
    group.append(el('h2', '', SECTION_TITLES[section] ?? section));
    const row = el('div', 'knobs');
    group.append(row);
    container.append(group);

    for (const q of list) {
      const node = el('div', 'knob');
      node.tabIndex = 0;
      node.setAttribute('role', 'slider');
      node.setAttribute('aria-label', q.label);
      const label = el('label', '', q.label);
      const canvas = el('canvas');
      const out = el('output');
      node.append(label, canvas, out);
      row.append(node);

      const draw = () => {
        const ctx = canvas.getContext('2d');
        const size = 56, dpr = window.devicePixelRatio || 1;
        if (canvas.width !== size * dpr) { canvas.width = canvas.height = size * dpr; }
        ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
        ctx.clearRect(0, 0, size, size);

        const norm = valueToNorm(q, values[q.id]);
        const cx = size / 2, cy = size / 2, r = size / 2 - 6;
        const angle = KNOB_START + norm * (KNOB_END - KNOB_START);

        ctx.lineCap = 'round';
        ctx.lineWidth = 3;
        ctx.strokeStyle = C.track;
        ctx.beginPath(); ctx.arc(cx, cy, r, KNOB_START, KNOB_END); ctx.stroke();
        ctx.strokeStyle = C.accent;
        ctx.beginPath(); ctx.arc(cx, cy, r, KNOB_START, angle); ctx.stroke();

        ctx.fillStyle = C.body;
        ctx.beginPath(); ctx.arc(cx, cy, r - 6, 0, 2 * Math.PI); ctx.fill();

        ctx.strokeStyle = C.text;
        ctx.lineWidth = 2.5;
        ctx.beginPath();
        ctx.moveTo(cx + Math.cos(angle) * (r - 15), cy + Math.sin(angle) * (r - 15));
        ctx.lineTo(cx + Math.cos(angle) * (r - 7), cy + Math.sin(angle) * (r - 7));
        ctx.stroke();

        out.textContent = formatValue(q, values[q.id]);
        node.setAttribute('aria-valuenow', String(values[q.id]));
        node.setAttribute('aria-valuetext', out.textContent);
      };

      const set = (value, fromUser) => {
        value = clamp(value, q.min, q.max);
        if (value === values[q.id]) return;
        values[q.id] = value;
        draw();
        if (fromUser) onChange(q.id, value);
      };

      const discrete = q.step >= 1;
      const nudge = (dir) => {
        if (discrete) set(clamp(Math.round(values[q.id]) + dir, q.min, q.max), true);
        else set(normToValue(q, valueToNorm(q, values[q.id]) + dir * 0.03), true);
      };

      let drag = null;
      node.addEventListener('pointerdown', (e) => {
        node.setPointerCapture(e.pointerId);
        drag = { y: e.clientY, norm: valueToNorm(q, values[q.id]), moved: false };
        node.focus();
      });
      node.addEventListener('pointermove', (e) => {
        if (!drag) return;
        const dy = drag.y - e.clientY;
        if (Math.abs(dy) > 3) drag.moved = true;
        if (!drag.moved) return;
        const scale = discrete ? 60 : (e.shiftKey ? 800 : 160);
        set(normToValue(q, drag.norm + dy / scale), true);
      });
      const end = () => {
        if (drag && !drag.moved && discrete) set(values[q.id] >= q.max ? q.min : Math.round(values[q.id]) + 1, true);
        drag = null;
      };
      node.addEventListener('pointerup', end);
      node.addEventListener('pointercancel', () => { drag = null; });
      node.addEventListener('dblclick', () => set(q.def, true));
      node.addEventListener('wheel', (e) => { e.preventDefault(); nudge(e.deltaY < 0 ? 1 : -1); }, { passive: false });
      node.addEventListener('keydown', (e) => {
        if (e.key === 'ArrowUp' || e.key === 'ArrowRight') { nudge(1); e.preventDefault(); }
        else if (e.key === 'ArrowDown' || e.key === 'ArrowLeft') { nudge(-1); e.preventDefault(); }
      });

      knobs.set(q.id, { draw });
      draw();
    }
  }

  return { refreshAll() { for (const k of knobs.values()) k.draw(); }, refresh(id) { knobs.get(id)?.draw(); } };
}

// ----------------------------------------------------------------- tastatur

const BLACK = new Set([1, 3, 6, 8, 10]);

export function createKeyboard(container, low, high, onDown, onUp) {
  const keys = new Map();
  let whiteCount = 0;
  for (let n = low; n <= high; n++) if (!BLACK.has(n % 12)) whiteCount++;
  container.style.setProperty('--whites', whiteCount);

  let whiteIndex = 0;
  for (let n = low; n <= high; n++) {
    const black = BLACK.has(n % 12);
    const key = el('div', 'key ' + (black ? 'black' : 'white'));
    key.dataset.note = n;
    if (black) key.style.left = `${(whiteIndex / whiteCount) * 100 - 30 / whiteCount}%`;
    else whiteIndex++;
    container.append(key);
    keys.set(n, key);
  }

  const velocityAt = (key, e) => {
    const r = key.getBoundingClientRect();
    return Math.round(40 + 87 * clamp((e.clientY - r.top) / r.height, 0, 1));
  };
  const keyAt = (e) => document.elementFromPoint(e.clientX, e.clientY)?.closest('.key');

  const held = new Map(); // pointerId -> note
  container.addEventListener('pointerdown', (e) => {
    const key = keyAt(e);
    if (!key) return;
    container.setPointerCapture(e.pointerId);
    const note = Number(key.dataset.note);
    held.set(e.pointerId, note);
    onDown(note, velocityAt(key, e));
  });
  container.addEventListener('pointermove', (e) => {
    if (!held.has(e.pointerId)) return;
    const key = keyAt(e);
    const note = key ? Number(key.dataset.note) : null;
    const prev = held.get(e.pointerId);
    if (note === prev || note === null) return;
    onUp(prev);
    held.set(e.pointerId, note);
    onDown(note, velocityAt(key, e));
  });
  const release = (e) => {
    if (!held.has(e.pointerId)) return;
    onUp(held.get(e.pointerId));
    held.delete(e.pointerId);
  };
  container.addEventListener('pointerup', release);
  container.addEventListener('pointercancel', release);

  return { setActive(note, on) { keys.get(note)?.classList.toggle('active', on); } };
}

// ---------------------------------------------------------------- bølgeform

export class WaveformView {
  // setParam(id, value): oppdaterer parameteren overalt (motor, knotter)
  constructor(canvas, titleEl, values, setParam) {
    this.canvas = canvas;
    this.titleEl = titleEl;
    this.values = values;
    this.setParam = setParam;
    this.sample = null;
    this.dirty = true;
    this.dragging = null;
    this.ctx = autoSize(canvas, () => { this.dirty = true; });

    canvas.addEventListener('pointerdown', (e) => {
      canvas.setPointerCapture(e.pointerId);
      const f = this.fraction(e);
      this.dragging = Math.abs(f - values.loopstart) < Math.abs(f - values.loopend) ? 'loopstart' : 'loopend';
      this.drag(e);
    });
    canvas.addEventListener('pointermove', (e) => { if (this.dragging) this.drag(e); });
    canvas.addEventListener('pointerup', () => { this.dragging = null; });
    canvas.addEventListener('pointercancel', () => { this.dragging = null; });
  }

  fraction(e) {
    const r = this.canvas.getBoundingClientRect();
    return clamp((e.clientX - r.left - 2) / Math.max(1, r.width - 4), 0, 1);
  }

  drag(e) {
    const f = this.fraction(e);
    if (this.dragging === 'loopstart') this.setParam('loopstart', clamp(f, 0, this.values.loopend - 0.001));
    else this.setParam('loopend', clamp(f, this.values.loopstart + 0.001, 1));
    this.setParam('loop', 1);
  }

  setSample(info) {
    this.sample = info;
    this.titleEl.textContent = info ? info.name : '';
    this.dirty = true;
  }

  invalidate() { this.dirty = true; }

  tick() {
    if (!this.dirty) return;
    this.dirty = false;
    const { ctx, canvas } = this;
    const w = canvas.clientWidth, h = canvas.clientHeight;
    ctx.clearRect(0, 0, w, h);

    const top = 16, bottom = h - 4, mid = (top + bottom) / 2, amp = (bottom - top) / 2;

    if (this.sample) {
      const { peaks } = this.sample;
      const cols = peaks.length / 2;
      ctx.fillStyle = C.accent;
      ctx.globalAlpha = 0.85;
      for (let x = 0; x < w; x++) {
        const c = Math.min(cols - 1, Math.floor((x / w) * cols));
        const y1 = mid - peaks[c * 2 + 1] * amp, y2 = mid - peaks[c * 2] * amp;
        ctx.fillRect(x, y1, 1, Math.max(1, y2 - y1));
      }
      ctx.globalAlpha = 1;
    } else {
      ctx.fillStyle = C.dim;
      ctx.font = '14px system-ui, sans-serif';
      ctx.textAlign = 'center';
      ctx.fillText('Ingen sample lastet', w / 2, h / 2);
    }

    const on = this.values.loop > 0.5;
    const x0 = 2 + this.values.loopstart * (w - 4), x1 = 2 + this.values.loopend * (w - 4);
    if (on) {
      ctx.fillStyle = C.accent2;
      ctx.globalAlpha = 0.14;
      ctx.fillRect(x0, top, x1 - x0, bottom - top);
    }
    ctx.globalAlpha = on ? 1 : 0.35;
    ctx.strokeStyle = C.accent2;
    ctx.lineWidth = 2;
    for (const x of [x0, x1]) { ctx.beginPath(); ctx.moveTo(x, top); ctx.lineTo(x, bottom); ctx.stroke(); }
    ctx.globalAlpha = 1;
  }
}

// ---------------------------------------------------------------- zone-kart

export class ZoneMap {
  constructor(canvas, onSelect) {
    this.canvas = canvas;
    this.onSelect = onSelect;
    this.instrument = null;
    this.selected = 0;
    this.dirty = true;
    this.ctx = autoSize(canvas, () => { this.dirty = true; });

    canvas.addEventListener('pointerdown', (e) => {
      if (!this.instrument) return;
      const r = canvas.getBoundingClientRect();
      const x = e.clientX - r.left, y = e.clientY - r.top;
      const zones = this.instrument.zones;
      for (let i = zones.length - 1; i >= 0; i--) {
        const b = this.bounds(zones[i]);
        if (x >= b.x && x <= b.x + b.w && y >= b.y && y <= b.y + b.h) {
          this.selected = i;
          this.dirty = true;
          this.onSelect(i);
          return;
        }
      }
    });
  }

  setInstrument(inst) { this.instrument = inst; this.selected = 0; this.dirty = true; }

  bounds(z) {
    const pad = 6, w = this.canvas.clientWidth - pad * 2, h = this.canvas.clientHeight - pad * 2;
    return {
      x: pad + (z.loKey / 128) * w,
      y: pad + ((127 - z.hiVel) / 127) * h,
      w: ((z.hiKey - z.loKey + 1) / 128) * w,
      h: ((z.hiVel - z.loVel + 1) / 127) * h,
    };
  }

  tick() {
    if (!this.dirty) return;
    this.dirty = false;
    const { ctx, canvas } = this;
    const w = canvas.clientWidth, h = canvas.clientHeight;
    ctx.clearRect(0, 0, w, h);

    ctx.strokeStyle = C.edge;
    ctx.lineWidth = 1;
    for (let k = 0; k <= 128; k += 12) {
      const x = 6 + ((w - 12) * k) / 128;
      ctx.beginPath(); ctx.moveTo(x, 6); ctx.lineTo(x, h - 6); ctx.stroke();
    }

    const inst = this.instrument;
    if (!inst || inst.zones.length === 0) {
      ctx.fillStyle = C.dim;
      ctx.font = '13px system-ui, sans-serif';
      ctx.textAlign = 'center';
      ctx.fillText('Ingen zones', w / 2, h / 2);
      return;
    }

    ctx.font = '10px system-ui, sans-serif';
    ctx.textAlign = 'left';
    inst.zones.forEach((z, i) => {
      const b = this.bounds(z);
      const hue = (z.sample * 49 + 170) % 360;
      ctx.fillStyle = `hsla(${hue}, 55%, 60%, 0.28)`;
      ctx.fillRect(b.x, b.y, b.w, b.h);
      ctx.strokeStyle = `hsla(${hue}, 55%, 60%, 0.85)`;
      ctx.lineWidth = 1;
      ctx.strokeRect(b.x + 0.5, b.y + 0.5, b.w - 1, b.h - 1);
      if (b.w > 46) {
        ctx.fillStyle = C.text;
        ctx.globalAlpha = 0.8;
        ctx.fillText(inst.samples[z.sample].name, b.x + 3, b.y + 11, b.w - 6);
        ctx.globalAlpha = 1;
      }
    });

    const sel = inst.zones[this.selected];
    if (sel) {
      const b = this.bounds(sel);
      ctx.strokeStyle = C.text;
      ctx.lineWidth = 2;
      ctx.strokeRect(b.x, b.y, b.w, b.h);
    }
  }
}

// ------------------------------------------------------------- nivåmåler

export class LevelMeter {
  constructor(canvas) {
    this.canvas = canvas;
    this.level = 0;
    this.shown = -1;
    this.ctx = autoSize(canvas, () => { this.shown = -1; });
  }

  push(peak) { this.level = Math.max(this.level, peak); }

  tick() {
    this.level *= 0.93;
    if (Math.abs(this.level - this.shown) < 0.002) return;
    this.shown = this.level;

    const { ctx, canvas } = this;
    const w = canvas.clientWidth, h = canvas.clientHeight;
    ctx.clearRect(0, 0, w, h);

    const db = 20 * Math.log10(Math.max(this.level, 1e-3));
    const frac = clamp((db + 60) / 60, 0, 1);
    const inner = h - 6;
    ctx.fillStyle = this.level >= 0.99 ? '#ef4444' : frac > 0.85 ? C.accent2 : C.accent;
    ctx.fillRect(3, 3 + inner * (1 - frac), w - 6, inner * frac);
  }
}
