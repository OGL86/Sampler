// Parametere. Samme id-er, grenser og standardverdier som Source/Params.h i C++-appen,
// slik at .samplerpreset-filer kan deles mellom appen og nettversjonen.

export const PARAMS = [
  // id, label, section, min, max, def, midpoint (0 = lineær), step, suffix
  p('attack',    'Attack',   'ENVELOPE', 0.001, 5.0,  0.005, 0.1,  0, ' s'),
  p('decay',     'Decay',    'ENVELOPE', 0.001, 5.0,  0.2,   0.3,  0, ' s'),
  p('sustain',   'Sustain',  'ENVELOPE', 0.0,   1.0,  0.8,   0,    0, ''),
  p('release',   'Release',  'ENVELOPE', 0.005, 8.0,  0.25,  0.4,  0, ' s'),
  p('velsens',   'Velocity', 'ENVELOPE', 0.0,   1.0,  0.7,   0,    0, ''),

  p('filtertype','Type',     'FILTER',   0, 2, 0, 0, 1, ''),
  p('cutoff',    'Cutoff',   'FILTER',   20, 20000, 12000, 1000, 0, ' Hz'),
  p('resonance', 'Reso',     'FILTER',   0.5, 12, 0.707, 2, 0, ''),
  p('pan',       'Pan',      'FILTER',   -1, 1, 0, 0, 0, ''),

  p('loop',      'Loop',     'LOOP',     0, 1, 0, 0, 1, ''),
  p('loopstart', 'Start',    'LOOP',     0, 1, 0, 0, 0, ''),
  p('loopend',   'End',      'LOOP',     0, 1, 1, 0, 0, ''),
  p('loopxfade', 'Xfade',    'LOOP',     0, 500, 10, 50, 0, ' ms'),

  p('lforate',   'Rate',     'LFO',      0.05, 20, 4, 2, 0, ' Hz'),
  p('lfopitch',  'Pitch',    'LFO',      0, 200, 0, 0, 0, ' ct'),
  p('lfofilter', 'Filter',   'LFO',      0, 4, 0, 0, 0, ' oct'),
  p('lfoamp',    'Tremolo',  'LFO',      0, 1, 0, 0, 0, ''),

  p('reverbmix', 'Reverb',   'EFFEKTER', 0, 1, 0, 0, 0, ''),
  p('reverbsize','Størrelse','EFFEKTER', 0, 1, 0.5, 0, 0, ''),
  p('delaymix',  'Delay',    'EFFEKTER', 0, 1, 0, 0, 0, ''),
  p('delaytime', 'Tid',      'EFFEKTER', 20, 1500, 350, 300, 0, ' ms'),
  p('delayfb',   'Feedback', 'EFFEKTER', 0, 0.95, 0.35, 0, 0, ''),

  p('master',    'Master',   'MASTER',   0, 1.5, 0.8, 0, 0, ''),
];

function p(id, label, section, min, max, def, midpoint, step, suffix) {
  return { id, label, section, min, max, def, midpoint, step, suffix };
}

export const SECTION_TITLES = {
  ENVELOPE: 'ENVELOPE', FILTER: 'FILTER & PAN', LOOP: 'LOOP',
  LFO: 'LFO', EFFEKTER: 'EFFEKTER', MASTER: 'MASTER',
};

export function defaultValues() {
  const v = {};
  for (const q of PARAMS) v[q.id] = q.def;
  return v;
}

// Knott-posisjon (0..1) <-> verdi, med samme skjevhet som JUCE sin setSkewFactorFromMidPoint.
function skewOf(q) {
  if (q.midpoint > q.min && q.midpoint < q.max) {
    return Math.log(0.5) / Math.log((q.midpoint - q.min) / (q.max - q.min));
  }
  return 1;
}

export function valueToNorm(q, value) {
  const prop = Math.min(1, Math.max(0, (value - q.min) / (q.max - q.min)));
  return Math.pow(prop, skewOf(q));
}

export function normToValue(q, norm) {
  const prop = Math.pow(Math.min(1, Math.max(0, norm)), 1 / skewOf(q));
  let v = q.min + (q.max - q.min) * prop;
  if (q.step > 0) v = q.min + Math.round((v - q.min) / q.step) * q.step;
  return v;
}

export function formatValue(q, value) {
  if (q.id === 'filtertype') return ['Av', 'Lavpass', 'Høypass'][Math.round(value)] ?? 'Av';
  if (q.id === 'loop') return value > 0.5 ? 'På' : 'Av';
  const decimals = q.step >= 1 ? 0 : q.max >= 100 ? 0 : q.max >= 10 ? 1 : 2;
  return value.toFixed(decimals) + q.suffix;
}
