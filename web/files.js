// Filhåndtering: innsamling av filer fra dra-og-slipp, matching av SFZ-stier,
// dekoding av lyd og lesing/skriving av presets (samme XML-format som C++-appen).

import { PARAMS } from './params.js';

const norm = (s) => s.toLowerCase().replaceAll('\\', '/');
const relPath = (f) => f.relPath ?? (f.webkitRelativePath || f.name);

export const baseName = (path) => path.replaceAll('\\', '/').split('/').pop();
export const extOf = (name) => (name.includes('.') ? name.split('.').pop().toLowerCase() : '');

// Må kalles synkront fra drop-hendelsen, før første await, ellers mister nettleseren tilgangen.
export async function collectFiles(dataTransfer) {
  const items = [...(dataTransfer.items ?? [])].filter((i) => i.kind === 'file');
  const entries = items.map((i) => i.webkitGetAsEntry?.()).filter(Boolean);
  if (entries.length === 0) return [...dataTransfer.files];

  const out = [];
  const walk = async (entry, path) => {
    if (entry.isFile) {
      const file = await new Promise((res, rej) => entry.file(res, rej));
      file.relPath = path + file.name;
      out.push(file);
    } else if (entry.isDirectory) {
      const reader = entry.createReader();
      let batch;
      do {
        batch = await new Promise((res, rej) => reader.readEntries(res, rej));
        for (const e of batch) await walk(e, path + entry.name + '/');
      } while (batch.length);
    }
  };
  for (const e of entries) await walk(e, '');
  return out;
}

// Finner filen en SFZ-sti peker på: først eksakt sti, ellers samme filnavn hvor som helst.
export function findFile(files, samplePath) {
  const p = norm(samplePath).replace(/^(\.\/)+/, '');
  const base = p.split('/').pop();
  let byBase = null;
  for (const f of files) {
    const r = norm(relPath(f));
    if (r === p || r.endsWith('/' + p)) return f;
    if (!byBase && r.split('/').pop() === base) byBase = f;
  }
  return byBase;
}

// Leser samplerate fra WAV-header (decodeAudioData resampler til konteksten, og SFZ-loop-punkter er i filens egne frames).
export function readWavRate(buf) {
  if (buf.byteLength < 28) return null;
  const dv = new DataView(buf);
  const tag = (o) => String.fromCharCode(dv.getUint8(o), dv.getUint8(o + 1), dv.getUint8(o + 2), dv.getUint8(o + 3));
  if (tag(0) !== 'RIFF' || tag(8) !== 'WAVE') return null;
  let p = 12;
  while (p + 8 <= buf.byteLength) {
    const size = dv.getUint32(p + 4, true);
    if (tag(p) === 'fmt ' && p + 16 <= buf.byteLength) return dv.getUint32(p + 12, true);
    p += 8 + size + (size & 1);
  }
  return null;
}

export async function decodeSample(ctx, file) {
  const data = await file.arrayBuffer();
  const originalRate = readWavRate(data); // må leses før decodeAudioData tar bufferen
  const audio = await ctx.decodeAudioData(data);
  const channels = [];
  for (let c = 0; c < Math.min(2, audio.numberOfChannels); c++) channels.push(audio.getChannelData(c).slice());
  return {
    name: file.name.replace(/\.[^.]+$/, ''),
    channels,
    sampleRate: audio.sampleRate,
    scale: originalRate ? audio.sampleRate / originalRate : 1,
    frames: audio.length,
  };
}

// Min/maks per kolonne, til bølgeformvisningen
export function computePeaks(channel, cols = 3000) {
  const peaks = new Float32Array(cols * 2);
  const span = channel.length / cols;
  for (let c = 0; c < cols; c++) {
    const from = Math.floor(c * span), to = Math.max(from + 1, Math.floor((c + 1) * span));
    let lo = 0, hi = 0;
    for (let i = from; i < to && i < channel.length; i++) {
      const v = channel[i];
      if (v < lo) lo = v;
      if (v > hi) hi = v;
    }
    peaks[c * 2] = lo;
    peaks[c * 2 + 1] = hi;
  }
  return peaks;
}

// ------------------------------------------------------------------ presets

const esc = (s) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');

export function presetToXml(values, instrumentName) {
  const attrs = PARAMS.map((q) => ` ${q.id}="${Number(values[q.id].toPrecision(8))}"`).join('');
  const inst = instrumentName ? ` instrument="${esc(instrumentName)}"` : '';
  return `<?xml version="1.0" encoding="UTF-8"?>\n<SamplerPreset version="1"${inst}${attrs}/>\n`;
}

// Returnerer { values, instrument } eller null hvis filen ikke er en gyldig preset
export function parsePresetXml(text) {
  const doc = new DOMParser().parseFromString(text, 'application/xml');
  const root = doc.documentElement;
  if (doc.querySelector('parsererror') || root.nodeName !== 'SamplerPreset') return null;

  const values = {};
  for (const q of PARAMS) {
    const raw = root.getAttribute(q.id);
    const v = raw === null ? NaN : parseFloat(raw);
    if (Number.isFinite(v)) values[q.id] = Math.min(q.max, Math.max(q.min, v));
  }
  const instrument = root.getAttribute('instrument');
  return { values, instrument: instrument ? baseName(instrument) : null };
}
