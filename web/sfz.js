// SFZ-parser. Samme logikk og samme støttede opcodes som Source/Instrument.cpp.
// Returnerer zones med samplePath (relativ sti slik den står i filen, inkl. default_path).

export function noteNameToNumber(textIn) {
  const text = String(textIn).trim().toLowerCase();
  if (!text) return -1;

  if (/^-?\d+$/.test(text)) {
    const n = parseInt(text, 10);
    return n >= 0 && n <= 127 ? n : -1;
  }

  const semitones = { a: 9, b: 11, c: 0, d: 2, e: 4, f: 5, g: 7 };
  const m = /^([a-g])([#b]?)(-?\d+)$/.exec(text);
  if (!m) return -1;

  const semi = semitones[m[1]] + (m[2] === '#' ? 1 : m[2] === 'b' ? -1 : 0);
  const n = (parseInt(m[3], 10) + 1) * 12 + semi;
  return n >= 0 && n <= 127 ? n : -1;
}

export function parseSfz(text) {
  let control = {}, global = {}, master = {}, group = {}, region = {};
  let current = null;     // 'control' | 'global' | 'master' | 'group' | 'region'
  let lastKey = null;
  let haveRegion = false;
  const zones = [];

  const target = () => ({ control, global, master, group, region })[current];

  const flushRegion = () => {
    if (!haveRegion) return;
    haveRegion = false;

    const ops = { ...global, ...master, ...group, ...region };
    if (!ops.sample) return;

    const num = (k, d) => (k in ops && !Number.isNaN(parseFloat(ops[k])) ? parseFloat(ops[k]) : d);
    const note = (k, d) => {
      if (!(k in ops)) return d;
      const n = noteNameToNumber(ops[k]);
      return n >= 0 ? n : d;
    };

    const keyOp = note('key', -1);
    const zone = {
      samplePath: ((control.default_path ?? '') + ops.sample).replaceAll('\\', '/'),
      loKey: note('lokey', keyOp >= 0 ? keyOp : 0),
      hiKey: note('hikey', keyOp >= 0 ? keyOp : 127),
      rootKey: note('pitch_keycenter', keyOp >= 0 ? keyOp : 60),
      loVel: clamp(note('lovel', 1), 0, 127),
      hiVel: clamp(note('hivel', 127), 0, 127),
      tuneCents: num('tune', 0) + 100 * num('transpose', 0),
      volumeDb: num('volume', 0),
      pan: clamp(num('pan', 0) / 100, -1, 1),
      seqLength: Math.max(1, Math.trunc(num('seq_length', 1))),
      seqPosition: 1,
      loopMode: 'params',            // 'params' | 'off' | 'on'
      loopStart: -1,
      loopEnd: -1,
    };
    zone.seqPosition = clamp(Math.trunc(num('seq_position', 1)), 1, zone.seqLength);

    const lm = ops.loop_mode;
    if (lm === 'loop_continuous' || lm === 'loop_sustain') zone.loopMode = 'on';
    else if (lm === 'no_loop' || lm === 'one_shot') zone.loopMode = 'off';

    if ('loop_start' in ops) zone.loopStart = Math.trunc(num('loop_start', -1));
    if ('loop_end' in ops) zone.loopEnd = Math.trunc(num('loop_end', -1));

    zones.push(zone);
  };

  const handleToken = (token) => {
    if (token.startsWith('<')) {
      const close = token.indexOf('>');
      if (close < 0) return;
      const name = token.slice(1, close).toLowerCase();
      const rest = token.slice(close + 1);

      flushRegion();

      if (name === 'region')       { region = {}; current = 'region'; haveRegion = true; }
      else if (name === 'group')   { group = {}; current = 'group'; }
      else if (name === 'master')  { master = {}; group = {}; current = 'master'; }
      else if (name === 'global')  { global = {}; master = {}; group = {}; current = 'global'; }
      else if (name === 'control') { current = 'control'; }
      else                         { current = null; } // <curve>, <effect> osv. ignoreres

      lastKey = null;
      if (rest) handleToken(rest);
      return;
    }

    const eq = token.indexOf('=');
    if (eq > 0) {
      lastKey = token.slice(0, eq).toLowerCase();
      const t = target();
      if (t) t[lastKey] = token.slice(eq + 1);
    } else if (token) {
      // Del av en verdi med mellomrom, f.eks. "sample=my sample.wav"
      const t = target();
      if (t && lastKey) t[lastKey] += ' ' + token;
    }
  };

  for (let line of text.split(/\r?\n/)) {
    const comment = line.indexOf('//');
    if (comment >= 0) line = line.slice(0, comment);
    for (const token of line.split(/[ \t]+/)) if (token) handleToken(token);
  }
  flushRegion();

  return zones;
}

function clamp(v, lo, hi) { return Math.min(hi, Math.max(lo, v)); }
