// Kjør med: node --test web/tests/
// Speiler Tests/EngineTests.cpp: pitch, release, loop, SFZ, round-robin og parametere.

import test from 'node:test';
import assert from 'node:assert/strict';

import { SamplerEngine } from '../engine.js';
import { parseSfz, noteNameToNumber } from '../sfz.js';
import { PARAMS, valueToNorm, normToValue } from '../params.js';

const SR = 44100;
const BLOCK = 128; // samme kvantum som en AudioWorklet

function sine(freq, seconds, amp = 0.8) {
  const a = new Float32Array(Math.floor(seconds * SR));
  for (let i = 0; i < a.length; i++) a[i] = amp * Math.sin((2 * Math.PI * freq * i) / SR);
  return { channels: [a], sampleRate: SR };
}

function zone(sample, over = {}) {
  return {
    sample, loKey: 0, hiKey: 127, loVel: 1, hiVel: 127, rootKey: 60, tuneCents: 0, volumeDb: 0,
    pan: 0, seqLength: 1, seqPosition: 1, loopMode: 'params', loopStart: -1, loopEnd: -1, ...over,
  };
}

function makeEngine(samples, zones, params = {}) {
  const e = new SamplerEngine(SR);
  e.setParam('attack', 0.001);
  e.setParam('release', 0.05);
  e.setParam('sustain', 1.0);
  for (const [k, v] of Object.entries(params)) e.setParam(k, v);
  e.loadInstrument({ samples, zones });
  return e;
}

// events: [{ at: sampleIndex, fn: (engine) => void }]
function run(engine, total, events = []) {
  const out = new Float32Array(total);
  const L = new Float32Array(BLOCK), R = new Float32Array(BLOCK);
  for (let start = 0; start < total; start += BLOCK) {
    for (const ev of events) if (ev.at >= start && ev.at < start + BLOCK) ev.fn(engine);
    engine.render(L, R, BLOCK);
    out.set(L.subarray(0, Math.min(BLOCK, total - start)), start);
  }
  return out;
}

const peak = (a, from, to) => { let p = 0; for (let i = from; i < Math.min(to, a.length); i++) p = Math.max(p, Math.abs(a[i])); return p; };
const finite = (a) => a.every(Number.isFinite);
function freq(a, from, to) {
  let c = 0;
  for (let i = from + 1; i < to; i++) if (a[i - 1] <= 0 && a[i] > 0) c++;
  return (c / (to - from)) * SR;
}

const on = (note) => ({ fn: (e) => e.noteOn(note, 127) });
const off = (note) => ({ fn: (e) => e.noteOff(note) });
const at = (t, ev) => ({ at: t, ...ev });

test('noteparsing', () => {
  assert.equal(noteNameToNumber('c4'), 60);
  assert.equal(noteNameToNumber('A0'), 21);
  assert.equal(noteNameToNumber('c#4'), 61);
  assert.equal(noteNameToNumber('db4'), 61);
  assert.equal(noteNameToNumber('72'), 72);
  assert.equal(noteNameToNumber('xyz'), -1);
  assert.equal(noteNameToNumber('200'), -1);
});

test('pitch: note 60 er original, note 72 er en oktav opp', () => {
  const s = sine(440, 1);
  const a = run(makeEngine([s], [zone(0)]), 12000, [at(0, on(60))]);
  assert.ok(finite(a));
  assert.ok(peak(a, 1000, 5000) > 0.2);
  assert.ok(Math.abs(freq(a, 1000, 9000) - 440) < 15, `f60=${freq(a, 1000, 9000)}`);

  const b = run(makeEngine([s], [zone(0)]), 12000, [at(0, on(72))]);
  assert.ok(Math.abs(freq(b, 1000, 9000) - 880) < 30, `f72=${freq(b, 1000, 9000)}`);
});

test('release og sample-slutt', () => {
  const s = sine(440, 1);
  const a = run(makeEngine([s], [zone(0)]), 44100, [at(0, on(60)), at(4096, off(60))]);
  assert.ok(peak(a, 1000, 4000) > 0.2, 'lyd mens tasten holdes');
  assert.ok(peak(a, 20000, 44100) < 0.001, 'stillhet etter release');

  const b = run(makeEngine([s], [zone(0)]), 60000, [at(0, on(60))]);
  assert.ok(peak(b, 50000, 60000) < 0.001, 'one-shot slutter');
});

test('loop med crossfade holder tonen gående', () => {
  const s = sine(440, 1);
  const e = makeEngine([s], [zone(0)], { loop: 1, loopstart: 0.1, loopend: 0.9, loopxfade: 10 });
  const a = run(e, SR * 3, [at(0, on(60))]);
  assert.ok(finite(a));
  assert.ok(peak(a, SR * 3 - 4096, SR * 3) > 0.2, 'lyd etter 3 s');
  assert.ok(Math.abs(freq(a, SR * 2, SR * 2 + 8000) - 440) < 20);
});

test('sustain-pedal holder tonen til pedalen slippes', () => {
  const s = sine(440, 3);
  const e = makeEngine([s], [zone(0)]);
  const a = run(e, SR, [
    at(0, { fn: (x) => x.setSustain(true) }), at(100, on(60)), at(2000, off(60)),
    at(SR - 8000, { fn: (x) => x.setSustain(false) }),
  ]);
  assert.ok(peak(a, 10000, 20000) > 0.2, 'klinger mens pedalen er nede');
  assert.ok(peak(a, SR - 2000, SR) < 0.001, 'stille etter at pedalen er sluppet');
});

test('filter, LFO og effekter gir gyldig lyd', () => {
  const s = sine(440, 2);
  const e = makeEngine([s], [zone(0)], {
    filtertype: 1, cutoff: 800, lfopitch: 50, lfofilter: 1, lfoamp: 0.3,
    delaymix: 0.4, reverbmix: 0.4, pan: -0.5,
  });
  const a = run(e, SR, [at(0, on(60))]);
  assert.ok(finite(a));
  assert.ok(peak(a, 5000, SR) > 0.05 && peak(a, 0, SR) <= 1.0);
});

test('SFZ: parsing', () => {
  const zones = parseSfz(
    '// kommentar\n<control>\ndefault_path=samples/\n<global> volume=-6\n' +
    '<group> lokey=c3 hikey=c5 pitch_keycenter=c4\n' +
    '<region> sample=a.wav seq_length=2 seq_position=1\n' +
    '<region> sample=b.wav seq_length=2 seq_position=2\n' +
    '<group> key=g2 lovel=64\n' +
    '<region> sample=my sample.wav tune=50 pan=-100 loop_mode=loop_continuous loop_start=100 loop_end=2000\n');
  assert.equal(zones.length, 3);
  const [z0, , z2] = zones;
  assert.deepEqual([z0.loKey, z0.hiKey, z0.rootKey], [48, 72, 60]);
  assert.deepEqual([z0.seqLength, z0.seqPosition], [2, 1]);
  assert.equal(z0.volumeDb, -6);
  assert.equal(z0.samplePath, 'samples/a.wav');
  assert.deepEqual([z2.loKey, z2.hiKey, z2.rootKey, z2.loVel], [43, 43, 43, 64]);
  assert.equal(z2.samplePath, 'samples/my sample.wav');
  assert.equal(z2.tuneCents, 50);
  assert.equal(z2.pan, -1);
  assert.deepEqual([z2.loopMode, z2.loopStart, z2.loopEnd], ['on', 100, 2000]);
});

test('round-robin bytter sample, og zones utenfor området gir stillhet', () => {
  const a = sine(440, 1), b = sine(880, 1);
  const zones = [
    zone(0, { loKey: 48, hiKey: 72, seqLength: 2, seqPosition: 1 }),
    zone(1, { loKey: 48, hiKey: 72, seqLength: 2, seqPosition: 2 }),
  ];
  const e = makeEngine([a, b], zones);
  const r = run(e, 30000, [at(0, on(60)), at(6000, off(60)), at(20000, on(60))]);
  assert.ok(Math.abs(freq(r, 1000, 5000) - 440) < 25, 'første anslag: a');
  assert.ok(Math.abs(freq(r, 21000, 25000) - 880) < 50, 'andre anslag: b');

  const none = run(makeEngine([a, b], zones), 4096, [at(0, on(100))]);
  assert.ok(peak(none, 0, 4096) < 0.001);
});

test('velocity-lag velger riktig zone', () => {
  const a = sine(440, 1), b = sine(880, 1);
  const zones = [zone(0, { loVel: 1, hiVel: 63 }), zone(1, { loVel: 64, hiVel: 127 })];
  const soft = run(makeEngine([a, b], zones), 6000, [at(0, { fn: (e) => e.noteOn(60, 30) })]);
  const hard = run(makeEngine([a, b], zones), 6000, [at(0, { fn: (e) => e.noteOn(60, 120) })]);
  assert.ok(Math.abs(freq(soft, 500, 5500) - 440) < 30);
  assert.ok(Math.abs(freq(hard, 500, 5500) - 880) < 50);
});

test('parametere: midtpunkt havner midt på knotten og verdier rundtur', () => {
  for (const q of PARAMS) {
    if (q.midpoint > q.min) assert.ok(Math.abs(valueToNorm(q, q.midpoint) - 0.5) < 1e-9, q.id);
    if (q.step === 0) {
      const back = normToValue(q, valueToNorm(q, q.def));
      assert.ok(Math.abs(back - q.def) < 1e-6 * (q.max - q.min) + 1e-9, q.id);
    }
  }
});

// ---- files.js (alt som ikke trenger DOM) ----
import { readWavRate, findFile, computePeaks, presetToXml, baseName } from '../files.js';
import { defaultValues } from '../params.js';

test('files: samplerate leses fra WAV-header', () => {
  const header = new Uint8Array(44);
  const dv = new DataView(header.buffer);
  header.set([0x52, 0x49, 0x46, 0x46], 0); dv.setUint32(4, 36, true);
  header.set([0x57, 0x41, 0x56, 0x45], 8);
  header.set([0x66, 0x6d, 0x74, 0x20], 12); dv.setUint32(16, 16, true);
  dv.setUint16(20, 1, true); dv.setUint16(22, 1, true); dv.setUint32(24, 48000, true);
  assert.equal(readWavRate(header.buffer), 48000);
  assert.equal(readWavRate(new ArrayBuffer(10)), null);
});

test('files: SFZ-stier matcher på sti først, filnavn som reserve', () => {
  const f = (relPath) => ({ relPath, name: relPath.split('/').pop() });
  const files = [f('lib/samples/A.wav'), f('lib/other/b.WAV'), f('lib/my sample.wav')];
  assert.equal(findFile(files, 'samples/a.wav'), files[0]);
  assert.equal(findFile(files, '../x/B.wav'), files[1]);
  assert.equal(findFile(files, 'my sample.wav'), files[2]);
  assert.equal(findFile(files, 'finnes-ikke.wav'), null);
  assert.equal(baseName('C:\\musikk\\piano.sfz'), 'piano.sfz');
});

test('files: peaks og preset-XML', () => {
  const a = new Float32Array(1000); a[10] = 0.5; a[510] = -0.25;
  const p = computePeaks(a, 10);
  assert.equal(p.length, 20);
  assert.equal(p[1], 0.5);        // maks i kolonne 0
  assert.equal(p[10], -0.25);     // min i kolonne 5

  const xml = presetToXml(defaultValues(), 'Piano & "Co"');
  assert.match(xml, /^<\?xml version="1.0" encoding="UTF-8"\?>\n<SamplerPreset version="1" instrument="Piano &amp; &quot;Co&quot;"/);
  assert.match(xml, /attack="0.005"/);
  assert.match(xml, /cutoff="12000"/);
});
