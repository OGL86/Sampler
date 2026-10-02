// Sampler for nettleseren. Binder sammen lyd (AudioWorklet), MIDI, filer, presets og grensesnitt.

import { PARAMS, defaultValues } from './params.js';
import { parseSfz } from './sfz.js';
import { createKnobs, createKeyboard, WaveformView, ZoneMap, LevelMeter } from './ui.js';
import {
  collectFiles, findFile, decodeSample, computePeaks, extOf, baseName, presetToXml, parsePresetXml,
} from './files.js';

const $ = (id) => document.getElementById(id);
const AUDIO_EXT = new Set(['wav', 'aif', 'aiff', 'flac', 'ogg', 'mp3', 'm4a', 'opus', 'webm']);

const values = defaultValues();
let ctx = null;
let node = null;
let audioReady = null;
let loading = false;
let instrument = null; // { name, samples: [{ name, frames, peaks }], zones }

const setStatus = (text) => { $('status').textContent = text; };
const send = (msg, transfer) => node?.port.postMessage(msg, transfer ?? []);

// ------------------------------------------------------------------- lyd

function ensureAudio() {
  if (!audioReady) audioReady = startAudio();
  else if (ctx && ctx.state === 'suspended') ctx.resume();
  return audioReady;
}

async function startAudio() {
  if (!window.AudioContext || !('audioWorklet' in AudioContext.prototype)) {
    setStatus('Nettleseren støtter ikke AudioWorklet. Bruk en nyere Chrome, Edge, Firefox eller Safari.');
    throw new Error('AudioWorklet mangler');
  }
  if (location.protocol === 'file:') {
    setStatus('Åpne siden via en webserver (f.eks. "py -m http.server" i web-mappen), ikke direkte fra filen.');
    throw new Error('file://');
  }

  ctx = new AudioContext({ latencyHint: 'interactive' });
  await ctx.audioWorklet.addModule('worklet.js');
  node = new AudioWorkletNode(ctx, 'sampler', { numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2] });
  node.port.onmessage = ({ data }) => { if (data.type === 'level') meter.push(data.peak); };
  node.connect(ctx.destination);

  for (const q of PARAMS) send({ type: 'param', id: q.id, value: values[q.id] });
  initMidi();
  return ctx;
}

// ------------------------------------------------------------- parametere

let knobs, wave, zoneMap, meter, keyboard;

function setParam(id, value) {
  values[id] = value;
  knobs.refresh(id);
  send({ type: 'param', id, value });
  if (id.startsWith('loop')) wave.invalidate();
}

// ------------------------------------------------------------------ noter

const down = new Set();

function noteOn(note, velocity = 100) {
  down.add(note);
  keyboard.setActive(note, true);
  if (node) { send({ type: 'noteOn', note, velocity }); return; }
  ensureAudio().then(() => { if (down.has(note)) send({ type: 'noteOn', note, velocity }); }).catch(() => {});
}

function noteOff(note) {
  down.delete(note);
  keyboard.setActive(note, false);
  send({ type: 'noteOff', note });
}

// ------------------------------------------------------------------- MIDI

async function initMidi() {
  if (!navigator.requestMIDIAccess) return;
  try {
    const access = await navigator.requestMIDIAccess();
    const bind = () => { for (const input of access.inputs.values()) input.onmidimessage = onMidi; };
    bind();
    access.onstatechange = bind;
  } catch { /* brukeren sa nei, eller MIDI er ikke tilgjengelig */ }
}

function onMidi({ data }) {
  const [status, d1, d2] = data;
  const cmd = status & 0xf0;
  if (cmd === 0x90 && d2 > 0) noteOn(d1, d2);
  else if (cmd === 0x80 || cmd === 0x90) noteOff(d1);
  else if (cmd === 0xb0 && d1 === 64) send({ type: 'sustain', down: d2 >= 64 });
  else if (cmd === 0xb0 && (d1 === 120 || d1 === 123)) send({ type: 'allOff' });
  else if (cmd === 0xe0) send({ type: 'bend', semitones: (((d2 << 7) | d1) - 8192) / 8192 * 2 });
}

// Datamaskintastatur
const KEYMAP = {
  KeyA: 0, KeyW: 1, KeyS: 2, KeyD: 4, KeyE: 3, KeyF: 5, KeyT: 6, KeyG: 7, KeyY: 8, KeyH: 9,
  KeyU: 10, KeyJ: 11, KeyK: 12, KeyO: 13, KeyL: 14, KeyP: 15, Semicolon: 16,
};
let octave = 0;
const typed = new Map();

window.addEventListener('keydown', (e) => {
  if (e.repeat || e.ctrlKey || e.metaKey || e.altKey) return;
  if (e.code === 'KeyZ') { octave = Math.max(-3, octave - 1); return; }
  if (e.code === 'KeyX') { octave = Math.min(3, octave + 1); return; }
  if (!(e.code in KEYMAP)) return;
  const note = 60 + octave * 12 + KEYMAP[e.code];
  if (note < 0 || note > 127) return;
  typed.set(e.code, note);
  noteOn(note, 100);
});

window.addEventListener('keyup', (e) => {
  const note = typed.get(e.code);
  if (note === undefined) return;
  typed.delete(e.code);
  noteOff(note);
});

// -------------------------------------------------------------- instrument

function defaultZone(sample) {
  return {
    sample, loKey: 0, hiKey: 127, loVel: 1, hiVel: 127, rootKey: 60, tuneCents: 0, volumeDb: 0,
    pan: 0, seqLength: 1, seqPosition: 1, loopMode: 'params', loopStart: -1, loopEnd: -1,
  };
}

async function loadFiles(files) {
  if (files.length === 0 || loading) return;

  const presets = files.filter((f) => extOf(f.name) === 'samplerpreset');
  const sfz = files.find((f) => extOf(f.name) === 'sfz');
  const audio = files.filter((f) => AUDIO_EXT.has(extOf(f.name)));

  if (!sfz && audio.length === 0) {
    if (presets.length) return loadPresetFile(presets[0]);
    setStatus('Fant ingen lydfiler eller .sfz-fil.');
    return;
  }

  loading = true;
  try {
    await ensureAudio();
    const result = sfz ? await buildFromSfz(sfz, files) : await buildFromAudio(audio);
    applyInstrument(result);
    if (presets.length) await loadPresetFile(presets[0], result.name);
  } catch (err) {
    if (err.message !== 'AudioWorklet mangler' && err.message !== 'file://') {
      setStatus('Kunne ikke laste: ' + err.message);
      console.error(err);
    }
  } finally {
    loading = false;
  }
}

async function buildFromAudio(audio) {
  setStatus('Laster ' + audio[0].name + ' …');
  const d = await decodeSample(ctx, audio[0]);
  const note = audio.length > 1 ? ` (bruker bare ${audio[0].name}; legg ved en .sfz for flere samples)` : '';
  return { name: d.name, decoded: [d], zones: [defaultZone(0)], message: note };
}

async function buildFromSfz(sfzFile, files) {
  const defs = parseSfz(await sfzFile.text());
  if (defs.length === 0) throw new Error('Fant ingen regioner i ' + sfzFile.name);

  const index = new Map(); // sti -> indeks i decoded (eller -1 hvis den mangler)
  const decoded = [];
  const warnings = [];
  const paths = [...new Set(defs.map((d) => d.samplePath))];

  for (const [i, path] of paths.entries()) {
    setStatus(`Laster ${i + 1}/${paths.length}: ${baseName(path)} …`);
    const file = findFile(files, path);
    if (!file) { warnings.push('mangler ' + baseName(path)); index.set(path, -1); continue; }
    try {
      decoded.push(await decodeSample(ctx, file));
      index.set(path, decoded.length - 1);
    } catch {
      warnings.push('kunne ikke dekode ' + baseName(path));
      index.set(path, -1);
    }
  }

  const zones = [];
  for (const d of defs) {
    const si = index.get(d.samplePath);
    if (si < 0) continue;
    const scale = decoded[si].scale;
    zones.push({
      ...d, sample: si,
      loopStart: d.loopStart >= 0 ? Math.round(d.loopStart * scale) : -1,
      loopEnd: d.loopEnd >= 0 ? Math.round(d.loopEnd * scale) : -1,
    });
  }
  if (zones.length === 0) throw new Error(warnings.join('; ') || 'ingen samples funnet');

  const name = sfzFile.name.replace(/\.[^.]+$/, '');
  return { name, decoded, zones, message: warnings.length ? ` (${warnings.join('; ')})` : '' };
}

function applyInstrument({ name, decoded, zones, message }) {
  // Bølgeform-data må beregnes før bufferne overføres til lydtråden
  const info = decoded.map((d) => ({ name: d.name, frames: d.frames, peaks: computePeaks(d.channels[0]) }));
  const samples = decoded.map((d) => ({ channels: d.channels, sampleRate: d.sampleRate }));
  const transfer = decoded.flatMap((d) => d.channels.map((c) => c.buffer));
  send({ type: 'instrument', samples, zones }, transfer);

  instrument = { name, samples: info, zones };
  zoneMap.setInstrument(instrument);
  wave.setSample(info[zones[0].sample]);
  setStatus(`${name}  |  ${zones.length} ${zones.length === 1 ? 'zone' : 'zones'}${message ?? ''}`);
}

// ------------------------------------------------------------------ presets

function savePreset() {
  const name = `${instrument?.name ?? 'sampler'}.samplerpreset`;
  const text = $('preset-text');
  text.value = presetToXml(values, instrument?.name);
  $('preset-name').textContent = name;
  $('preset-box').hidden = false;
  text.focus();
  text.select();

  // Nedlasting virker ikke i alle visninger (f.eks. innebygde sider), så teksten vises alltid også.
  try {
    const url = URL.createObjectURL(new Blob([text.value], { type: 'application/xml' }));
    const a = Object.assign(document.createElement('a'), { href: url, download: name });
    document.body.append(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  } catch { /* ignoreres */ }
  setStatus('Preset klar. Kopier teksten og lagre den som ' + name + ', eller bruk filen som ble lastet ned.');
}

async function loadPresetFile(file, currentName = instrument?.name) {
  applyPresetText(await file.text(), file.name, currentName);
}

function applyPresetText(text, label, currentName = instrument?.name) {
  const parsed = parsePresetXml(text);
  if (!parsed) { setStatus('Ugyldig preset: ' + label); return; }
  for (const [id, v] of Object.entries(parsed.values)) setParam(id, v);
  knobs.refreshAll();
  const hint = parsed.instrument && parsed.instrument !== currentName
    ? `  (forventer instrumentet ${parsed.instrument})` : '';
  setStatus('Preset lastet: ' + label + hint);
}


// --------------------------------------------------------------- oppsett

knobs = createKnobs($('knobs'), values, (id, value) => {
  send({ type: 'param', id, value });
  if (id.startsWith('loop')) wave.invalidate();
});
wave = new WaveformView($('wave'), $('wave-title'), values, setParam);
zoneMap = new ZoneMap($('zones'), (i) => {
  const z = instrument?.zones[i];
  if (z) wave.setSample(instrument.samples[z.sample]);
});
meter = new LevelMeter($('meter'));
keyboard = createKeyboard($('keyboard'), 36, 96, noteOn, noteOff);

const pick = (input) => () => input.click();
$('btn-files').onclick = pick($('in-files'));
$('btn-folder').onclick = pick($('in-folder'));
$('btn-preset').onclick = pick($('in-preset'));
$('btn-save').onclick = savePreset;
$('preset-close').onclick = () => { $('preset-box').hidden = true; };
$('preset-use').onclick = () => applyPresetText($('preset-text').value, 'limt inn tekst');
$('preset-copy').onclick = async () => {
  const text = $('preset-text');
  try { await navigator.clipboard.writeText(text.value); setStatus('Preset kopiert.'); }
  catch { text.focus(); text.select(); setStatus('Marker teksten og kopier den med Ctrl+C.'); }
};

for (const id of ['in-files', 'in-folder']) {
  $(id).addEventListener('change', (e) => { loadFiles([...e.target.files]); e.target.value = ''; });
}
$('in-preset').addEventListener('change', (e) => {
  if (e.target.files[0]) loadPresetFile(e.target.files[0]);
  e.target.value = '';
});

let dragDepth = 0;
window.addEventListener('dragenter', (e) => { e.preventDefault(); dragDepth++; $('drop').hidden = false; });
window.addEventListener('dragleave', () => { if (--dragDepth <= 0) { dragDepth = 0; $('drop').hidden = true; } });
window.addEventListener('dragover', (e) => e.preventDefault());
window.addEventListener('drop', (e) => {
  e.preventDefault();
  dragDepth = 0;
  $('drop').hidden = true;
  collectFiles(e.dataTransfer).then(loadFiles);
});

// Nettlesere krever en brukerhandling før lyd kan starte
window.addEventListener('pointerdown', () => ensureAudio().catch(() => {}), { once: true });

(function frame() {
  wave.tick();
  zoneMap.tick();
  meter.tick();
  requestAnimationFrame(frame);
})();
