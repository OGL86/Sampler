# Sampler

Standalone-sampler i C++17 med JUCE 8: polyfonisk avspilling av enkeltfiler og SFZ-instrumenter, med envelope, filter, loop, LFO, effekter, presets og disk-streaming av lange samples.

## Bygge

Krever CMake 3.22+ og en C++17-kompilator. JUCE hentes automatisk via FetchContent.

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

**Linux** trenger i tillegg utviklingspakker:

```
sudo apt install libasound2-dev libfreetype-dev libfontconfig1-dev \
  libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxext-dev libgl1-mesa-dev
```

Appen havner i `build/Sampler_artefacts/`.

### Tester

Motoren har tester uten GUI (pitch, release, loop, streaming, SFZ, round-robin):

```
cmake -B build -DSAMPLER_BUILD_TESTS=ON
cmake --build build --target SamplerTests
./build/SamplerTests_artefacts/SamplerTests
```

## Bruk

- **Last inn sample / SFZ...** eller slipp en lydfil (WAV/AIFF/FLAC/OGG/MP3), en `.sfz`-fil eller en `.samplerpreset` i vinduet.
- Spill med tilkoblet MIDI-keyboard (inkl. pitch bend og sustain-pedal), skjermtastaturet eller datamaskintastaturet.
- **Zone-kartet** viser hvordan instrumentet er lagt ut (taster x velocity). Klikk på en zone for å se samplet i bølgeformvisningen.
- **Loop**: slå på Loop-knotten og dra markørene i bølgeformen, eller bruk Start/End. Crossfade jevner ut skjøten.
- Dobbeltklikk på en knott for å nullstille den.
- **Lagre preset / Last preset** skriver XML (`.samplerpreset`) med alle knotter og stien til instrumentet.

## SFZ-støtte

`<control>` (`default_path`), `<global>`, `<master>`, `<group>`, `<region>` med:
`sample`, `key`, `lokey`, `hikey`, `pitch_keycenter`, `lovel`, `hivel`, `tune`, `transpose`, `volume`, `pan`,
`loop_mode`, `loop_start`, `loop_end`, `seq_length`, `seq_position`. Øvrige opcodes ignoreres.

## Arkitektur

| Fil | Ansvar |
|---|---|
| `Source/SamplerEngine.*`, `SamplerRender.cpp` | 64 stemmer: zone-valg, round-robin, ADSR, Hermite-interpolasjon, loop med crossfade, filter, pan, LFO, effekter. |
| `Source/Instrument.*` | Sample/Zone/Instrument og lasting av enkeltfiler og SFZ. |
| `Source/DiskStreamer.*` | Bakgrunnstråd som fyller en ringbuffer per stemme for samples lengre enn 12 s. |
| `Source/Params.h` | Alle parametere som atomics, med beskrivelse som GUI og presets bygges fra. |
| `Source/Effects.h`, `Biquad.h` | Stereo-delay og biquad-filter (reverb er `juce::Reverb`). |
| `Source/Ui.*`, `MainComponent.*` | Knotter, bølgeform, zone-kart, nivåmåler, MIDI og presets. |
| `Tests/EngineTests.cpp` | Motor-tester. |

### Regler for lydtråden

Ingen `new`/`malloc`, ingen mutex, ingen fil-I/O, ingen logging. Alt kommuniserer via atomics:

- Parametere: `AtomicFloat`, lest én gang per blokk.
- Instrument: publiseres via atomisk peker. Lydtråden stopper stemmene og kvitterer med en epoke, og først da frigjør meldingstråden det gamle instrumentet.
- Streaming: tilstanden `(generasjon << 40) | skriveposisjon` oppdateres med compare-exchange, så disk-tråden aldri publiserer data for en note som er retriggret. Ved underrun spilles stillhet i stedet for å blokkere.
- Sampleavspilling mikses med vektoriserte operasjoner (`FloatVectorOperations`).

## Kjente begrensninger

- Loop støttes ikke for samples som strømmes (lengre enn 12 s).
- Voice-stealing kutter den stjålne stemmen uten fade, så det kan høres et lite klikk når alle 64 stemmer er i bruk.
- Loop-parametrene leses ved note-on; endringer påvirker nye noter.
- Zone-kartet er skrivebeskyttet (zones redigeres i SFZ-filen).
- SFZ-opcodes for per-region envelope, filter og andre effekter ignoreres; de globale knottene gjelder alle regioner.
