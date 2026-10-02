// AudioWorklet-wrapper rundt SamplerEngine. Meldinger fra hovedtråden:
//   { type: 'instrument', samples, zones }   (Float32Array-ene overføres uten kopiering)
//   { type: 'param', id, value }
//   { type: 'noteOn', note, velocity } / { type: 'noteOff', note }
//   { type: 'sustain', down } / { type: 'bend', semitones } / { type: 'allOff' }
// Sender { type: 'level', peak } ca. 20 ganger i sekundet.

import { SamplerEngine } from './engine.js';

class SamplerProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.engine = new SamplerEngine(sampleRate);
    this.frames = 0;

    this.port.onmessage = ({ data: m }) => {
      const e = this.engine;
      switch (m.type) {
        case 'instrument': e.loadInstrument(m); break;
        case 'param':      e.setParam(m.id, m.value); break;
        case 'noteOn':     e.noteOn(m.note, m.velocity); break;
        case 'noteOff':    e.noteOff(m.note); break;
        case 'sustain':    e.setSustain(m.down); break;
        case 'bend':       e.pitchBend(m.semitones); break;
        case 'allOff':     e.allVoicesOff(); break;
      }
    };
  }

  process(_inputs, outputs) {
    const out = outputs[0];
    const n = out[0].length;
    this.engine.render(out[0], out[1] ?? out[0], n);

    this.frames += n;
    if (this.frames >= sampleRate / 20) {
      this.frames = 0;
      this.port.postMessage({ type: 'level', peak: this.engine.fetchPeak() });
    }
    return true;
  }
}

registerProcessor('sampler', SamplerProcessor);
