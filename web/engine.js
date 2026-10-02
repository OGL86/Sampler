// Sampler-motor for nettleseren. Port av Source/SamplerEngine.* og SamplerRender.cpp.
// Ren JavaScript uten Web Audio-avhengigheter, slik at den kan kjøres både i en
// AudioWorklet (se worklet.js) og i Node for tester.

import { defaultValues } from './params.js';

const HALF_PI = Math.PI / 2;
const SQRT2 = Math.SQRT2;
const FILTER_SUB = 32;
const MAX_BLOCK = 4096;

// ---------------------------------------------------------------- effekter

class Comb {
  constructor(size) { this.buf = new Float32Array(size); this.i = 0; this.store = 0; }
  process(input, feedback, damp) {
    const out = this.buf[this.i];
    this.store = out * (1 - damp) + this.store * damp;
    this.buf[this.i] = input + this.store * feedback;
    if (++this.i >= this.buf.length) this.i = 0;
    return out;
  }
}

class AllPass {
  constructor(size) { this.buf = new Float32Array(size); this.i = 0; }
  process(input) {
    const b = this.buf[this.i];
    const out = b - input;
    this.buf[this.i] = input + b * 0.5;
    if (++this.i >= this.buf.length) this.i = 0;
    return out;
  }
}

// Freeverb, samme oppsett som juce::Reverb
class Reverb {
  constructor(sampleRate) {
    const k = sampleRate / 44100;
    const combs = [1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617];
    const allpasses = [556, 441, 341, 225];
    const size = (n) => Math.max(8, Math.round(n * k));
    this.combL = combs.map((n) => new Comb(size(n)));
    this.combR = combs.map((n) => new Comb(size(n + 23)));
    this.apL = allpasses.map((n) => new AllPass(size(n)));
    this.apR = allpasses.map((n) => new AllPass(size(n + 23)));
  }

  process(L, R, n, roomSize, mix) {
    const feedback = roomSize * 0.28 + 0.7;
    const damp = 0.5 * 0.4;
    const wet = mix * 0.9;

    for (let i = 0; i < n; i++) {
      const input = (L[i] + R[i]) * 0.015;
      let oL = 0, oR = 0;
      for (const c of this.combL) oL += c.process(input, feedback, damp);
      for (const c of this.combR) oR += c.process(input, feedback, damp);
      for (const a of this.apL) oL = a.process(oL);
      for (const a of this.apR) oR = a.process(oR);
      L[i] += oL * wet;
      R[i] += oR * wet;
    }
  }
}

// Stereo ping-pong delay
class Delay {
  constructor(sampleRate) {
    this.rate = sampleRate;
    this.size = Math.floor(sampleRate * 2) + 4;
    this.l = new Float32Array(this.size);
    this.r = new Float32Array(this.size);
    this.w = 0;
  }

  process(L, R, n, timeMs, feedback, mix) {
    const d = Math.min(this.size - 2, Math.max(1, Math.floor(timeMs * 0.001 * this.rate)));
    for (let i = 0; i < n; i++) {
      let rp = this.w - d;
      if (rp < 0) rp += this.size;
      const dl = this.l[rp], dr = this.r[rp];
      this.l[this.w] = L[i] + dr * feedback;
      this.r[this.w] = R[i] + dl * feedback;
      L[i] += dl * mix;
      R[i] += dr * mix;
      if (++this.w >= this.size) this.w = 0;
    }
  }
}

// ------------------------------------------------------------------ stemme

class Voice {
  constructor() {
    this.active = false;
    this.releasing = false;
    this.sustainHeld = false;
    this.note = -1;
    this.age = 0;
    this.sample = null;
    this.pos = 0;
    this.baseInc = 1;
    this.gain = 1;
    this.zonePan = 0;
    this.looping = false;
    this.loopStart = 0;
    this.loopEnd = 0;
    this.xfade = 0;
    this.envState = 0; // 0 av, 1 attack, 2 decay, 3 sustain, 4 release
    this.env = 0;
    this.relRate = 0;
    this.z = new Float64Array(4); // filterminne: z1L, z2L, z1R, z2R
    this.c = new Float64Array(5); // koeffisienter b0..a2 (normalisert)
  }
}

function hermite(ym1, y0, y1, y2, t) {
  const c1 = 0.5 * (y1 - ym1);
  const c2 = ym1 - 2.5 * y0 + 2 * y1 - 0.5 * y2;
  const c3 = 0.5 * (y2 - ym1) + 1.5 * (y0 - y1);
  return ((c3 * t + c2) * t + c1) * t + y0;
}

function interp(a, pos, len) {
  const idx = Math.floor(pos);
  const t = pos - idx;
  const im = idx - 1, i1 = idx + 1, i2 = idx + 2;
  const ym1 = im < 0 ? a[0] : im >= len ? 0 : a[im];
  const y0 = idx < 0 ? a[0] : idx >= len ? 0 : a[idx];
  const y1 = i1 >= len ? 0 : a[i1];
  const y2 = i2 >= len ? 0 : a[i2];
  return hermite(ym1, y0, y1, y2, t);
}

// -------------------------------------------------------------------- motor

export class SamplerEngine {
  constructor(sampleRate, numVoices = 64) {
    this.sr = sampleRate;
    this.params = defaultValues();
    this.voices = Array.from({ length: numVoices }, () => new Voice());
    this.zones = [];
    this.samples = [];
    this.voiceCounter = 0;
    this.sustainDown = false;
    this.bendFactor = 1;
    this.roundRobin = new Int32Array(128);
    this.lfoPhase = 0;
    this.lastMaster = this.params.master;
    this.peak = 0;

    this.vl = new Float32Array(MAX_BLOCK);
    this.vr = new Float32Array(MAX_BLOCK);
    this.envBuf = new Float32Array(MAX_BLOCK);
    this.lfo = new Float32Array(MAX_BLOCK);
    this.delay = new Delay(sampleRate);
    this.reverb = new Reverb(sampleRate);
    this.snapshot();
  }

  // ---- styring ----

  setParam(id, value) { if (id in this.params) this.params[id] = value; }

  // instrument: { samples: [{ channels: [Float32Array, ...], sampleRate }], zones: [{ sample: index, ... }] }
  loadInstrument(inst) {
    this.allVoicesOff(true);
    this.samples = inst.samples.map((s) => {
      const l = s.channels[0];
      return { l, r: s.channels[1] ?? l, length: l.length, sampleRate: s.sampleRate };
    });
    this.zones = inst.zones.map((z) => ({ ...z, sample: this.samples[z.sample] }));
    this.roundRobin.fill(0);
  }

  noteOn(note, velocity) {
    if (note < 0 || note > 127) return;
    const rr = this.roundRobin[note];

    for (const z of this.zones) {
      if (note < z.loKey || note > z.hiKey || velocity < z.loVel || velocity > z.hiVel) continue;
      if ((rr % z.seqLength) + 1 !== z.seqPosition) continue;
      this.startVoice(this.allocateVoice(), z, note, velocity);
    }
    this.roundRobin[note] = (rr + 1) & 0xfffff;
  }

  noteOff(note) {
    for (const v of this.voices) {
      if (!v.active || v.releasing || v.sustainHeld || v.note !== note) continue;
      if (this.sustainDown) v.sustainHeld = true;
      else this.release(v);
    }
  }

  setSustain(down) {
    this.sustainDown = down;
    if (down) return;
    for (const v of this.voices) {
      if (v.active && v.sustainHeld) { v.sustainHeld = false; this.release(v); }
    }
  }

  pitchBend(semitones) { this.bendFactor = Math.pow(2, semitones / 12); }

  allVoicesOff(hard = false) {
    for (const v of this.voices) {
      if (!v.active) continue;
      if (hard) v.active = false;
      else if (!v.releasing) { v.sustainHeld = false; this.release(v); }
    }
  }

  fetchPeak() { const p = this.peak; this.peak = 0; return p; }

  // ---- stemmehåndtering ----

  release(v) {
    v.releasing = true;
    v.envState = 4;
    v.relRate = v.env / Math.max(1, this.p.release * this.sr);
  }

  allocateVoice() {
    let victim = this.voices.find((v) => !v.active);
    if (victim) return victim;

    for (const v of this.voices) if (v.releasing && (!victim || v.age < victim.age)) victim = v;
    if (victim) return victim;

    victim = this.voices[0];
    for (const v of this.voices) if (v.age < victim.age) victim = v;
    return victim;
  }

  startVoice(v, z, note, velocity) {
    const s = z.sample;
    v.active = true;
    v.releasing = false;
    v.sustainHeld = false;
    v.note = note;
    v.age = ++this.voiceCounter;
    v.sample = s;
    v.pos = 0;

    const semis = note - z.rootKey + z.tuneCents / 100;
    v.baseInc = (s.sampleRate / this.sr) * Math.pow(2, semis / 12);

    const velNorm = velocity / 127;
    const velFactor = 1 + this.p.velSens * (velNorm * velNorm - 1);
    v.gain = 0.5 * velFactor * Math.pow(10, z.volumeDb / 20);
    v.zonePan = z.pan;

    let loopOn = z.loopMode === 'on' || (z.loopMode === 'params' && this.params.loop > 0.5);
    if (loopOn) {
      const total = s.length;
      let ls, le;
      if (z.loopStart >= 0 && z.loopEnd > z.loopStart) { ls = z.loopStart; le = z.loopEnd; }
      else { ls = this.params.loopstart * total; le = this.params.loopend * total; }

      le = Math.min(le, total - 1);
      ls = Math.max(0, Math.min(ls, le - 16));

      if (le - ls < 16) loopOn = false;
      else {
        const xf = this.params.loopxfade * 0.001 * s.sampleRate;
        v.loopStart = ls;
        v.loopEnd = le;
        v.xfade = Math.min(xf, ls, (le - ls) * 0.5);
      }
    }
    v.looping = loopOn;

    v.env = 0;
    v.envState = 1;
    v.z.fill(0);
  }

  // Leser parametrene én gang per blokk
  snapshot() {
    const q = this.params, sr = this.sr;
    const sus = Math.min(1, Math.max(0, q.sustain));
    this.p = {
      aRate: 1 / Math.max(1, Math.max(0.001, q.attack) * sr),
      dRate: (1 - sus) / Math.max(1, Math.max(0.001, q.decay) * sr),
      sustain: sus,
      release: Math.max(0.005, q.release),
      velSens: Math.min(1, Math.max(0, q.velsens)),
      filterType: Math.round(q.filtertype),
      cutoff: q.cutoff,
      reso: q.resonance,
      pan: q.pan,
      lfoRate: q.lforate,
      lfoPitch: q.lfopitch,
      lfoFilter: q.lfofilter,
      lfoAmp: q.lfoamp,
    };
  }

  // ---- rendering ----

  // Overskriver outL/outR for n samples
  render(outL, outR, n) {
    for (let start = 0; start < n; start += MAX_BLOCK) {
      const m = Math.min(MAX_BLOCK, n - start);
      this.renderChunk(start === 0 && m === n ? outL : outL.subarray(start, start + m),
                       start === 0 && m === n ? outR : outR.subarray(start, start + m), m);
    }
  }

  renderChunk(L, R, n) {
    this.snapshot();
    L.fill(0, 0, n);
    R.fill(0, 0, n);

    const phaseInc = this.p.lfoRate / this.sr;
    for (let i = 0; i < n; i++) {
      this.lfo[i] = Math.sin(2 * Math.PI * this.lfoPhase);
      this.lfoPhase += phaseInc;
      if (this.lfoPhase >= 1) this.lfoPhase -= 1;
    }

    for (const v of this.voices) if (v.active) this.renderVoice(v, L, R, n);

    const q = this.params;
    if (q.delaymix > 0.001) this.delay.process(L, R, n, q.delaytime, q.delayfb, q.delaymix);
    if (q.reverbmix > 0.001) this.reverb.process(L, R, n, q.reverbsize, q.reverbmix);

    const master = q.master;
    const step = (master - this.lastMaster) / n;
    let g = this.lastMaster;
    let peak = this.peak;
    for (let i = 0; i < n; i++) {
      g += step;
      const l = Math.max(-1, Math.min(1, L[i] * g));
      const r = Math.max(-1, Math.min(1, R[i] * g));
      L[i] = l; R[i] = r;
      const a = Math.max(Math.abs(l), Math.abs(r));
      if (a > peak) peak = a;
    }
    this.lastMaster = master;
    this.peak = peak;
  }

  renderVoice(v, L, R, n) {
    const p = this.p;
    const s = v.sample;
    const sl = s.l, sr_ = s.r, len = s.length;
    const vl = this.vl, vr = this.vr, eb = this.envBuf, lfo = this.lfo;
    const loopLen = v.loopEnd - v.loopStart;
    const vibrato = p.lfoPitch > 0.01;
    const vibDepth = p.lfoPitch / 1200;

    let produced = n;
    let ended = false;

    // 1) hent og interpoler
    for (let i = 0; i < n; i++) {
      if (v.looping) {
        while (v.pos >= v.loopEnd) v.pos -= loopLen;
      } else if (v.pos >= len - 1) {
        produced = i; ended = true; break;
      }

      let l = interp(sl, v.pos, len);
      let r = sl === sr_ ? l : interp(sr_, v.pos, len);

      if (v.looping && v.xfade > 1) {
        const fadeStart = v.loopEnd - v.xfade;
        if (v.pos >= fadeStart) {
          const t = (v.pos - fadeStart) / v.xfade;
          const p2 = v.pos - loopLen;
          const l2 = interp(sl, p2, len);
          const r2 = sl === sr_ ? l2 : interp(sr_, p2, len);
          const a = Math.cos(t * HALF_PI), b = Math.sin(t * HALF_PI);
          l = l * a + l2 * b;
          r = r * a + r2 * b;
        }
      }

      vl[i] = l; vr[i] = r;

      let inc = v.baseInc * this.bendFactor;
      if (vibrato) inc *= Math.pow(2, vibDepth * lfo[i]);
      v.pos += inc;
    }

    // 2) envelope og tremolo
    const tremolo = p.lfoAmp > 0.001;
    for (let i = 0; i < produced; i++) {
      switch (v.envState) {
        case 1: v.env += p.aRate; if (v.env >= 1) { v.env = 1; v.envState = 2; } break;
        case 2: v.env -= p.dRate; if (v.env <= p.sustain) { v.env = p.sustain; v.envState = 3; } break;
        case 3: v.env = p.sustain; break;
        case 4: v.env -= v.relRate; if (v.env <= 0) { v.env = 0; v.envState = 0; } break;
      }
      if (v.envState === 0) { produced = i; ended = true; break; }
      eb[i] = tremolo ? v.env * (1 - p.lfoAmp * (0.5 + 0.5 * lfo[i])) : v.env;
    }

    // 3) filter
    if (p.filterType !== 0 && produced > 0) {
      const z = v.z, c = v.c;
      for (let i = 0; i < produced; i += FILTER_SUB) {
        const m = Math.min(FILTER_SUB, produced - i);
        let cutoff = p.cutoff;
        if (p.lfoFilter > 0) cutoff *= Math.pow(2, p.lfoFilter * lfo[i]);
        this.coefficients(c, p.filterType === 1, cutoff, p.reso);

        for (let j = i; j < i + m; j++) {
          const xl = vl[j];
          const yl = c[0] * xl + z[0];
          z[0] = c[1] * xl - c[3] * yl + z[1];
          z[1] = c[2] * xl - c[4] * yl;
          vl[j] = yl;

          const xr = vr[j];
          const yr = c[0] * xr + z[2];
          z[2] = c[1] * xr - c[3] * yr + z[3];
          z[3] = c[2] * xr - c[4] * yr;
          vr[j] = yr;
        }
      }
    }

    // 4) miks inn med pan
    if (produced > 0) {
      const pan = Math.min(1, Math.max(-1, p.pan + v.zonePan));
      const angle = (pan + 1) * (HALF_PI * 0.5);
      const gl = Math.cos(angle) * SQRT2 * v.gain;
      const gr = Math.sin(angle) * SQRT2 * v.gain;
      for (let i = 0; i < produced; i++) {
        L[i] += vl[i] * eb[i] * gl;
        R[i] += vr[i] * eb[i] * gr;
      }
    }

    if (ended) v.active = false;
  }

  // RBJ biquad, samme som Source/Biquad.h
  coefficients(c, lowpass, freq, q) {
    freq = Math.min(Math.max(freq, 20), this.sr * 0.45);
    q = Math.max(q, 0.1);
    const w0 = 2 * Math.PI * freq / this.sr;
    const cosw = Math.cos(w0);
    const alpha = Math.sin(w0) / (2 * q);
    let b0, b1;
    if (lowpass) { b0 = (1 - cosw) / 2; b1 = 1 - cosw; }
    else { b0 = (1 + cosw) / 2; b1 = -(1 + cosw); }
    const a0 = 1 + alpha;
    c[0] = b0 / a0; c[1] = b1 / a0; c[2] = b0 / a0;
    c[3] = (-2 * cosw) / a0; c[4] = (1 - alpha) / a0;
  }
}
