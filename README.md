# SignalScope

A real-time SDR spectrum, waterfall, and multi-decoder console for Windows and linux.

> **Work in progress.** SignalScope is under active development (v0.0.2).
> Features, decoders, and APIs may change without notice, and things may break.

## Features

- Live spectrum + scrolling waterfall with adjustable FFT size and averaging
- Drop decoders directly on the spectrum (Ctrl+click) — each runs in its own
  down-converted channel across a worker-thread pool
- Band browsing (pan the view to retune the radio), band-plan overlays
- Audio output for broadcast/voice modes with volume, mute, and a level meter
- IQ recording with pre-buffer
- Dual-RTL mode (two independent receivers side by side)
- Dockable, persistent panel layout

## Supported SDRs

| Source            | Notes                                             |
|-------------------|---------------------------------------------------|
| RTL-SDR           | Including a dual-RTL mode                          |
| HackRF            |                                                   |
| Airspy R2 / Mini  | Optional (enabled when libairspy is found)        |
| LibreSDR / USRP B210 | Via UHD; loads a custom FPGA bitstream. RXA/RXB/TRXA/TRXB port selection |
| SDR++ server      | Network source                                    |
| WAV / IQ file     | Playback of recorded captures                     |

## Decoders

Decoders are pluggable "channel" types placed on the spectrum:

| Decoder | Description |
|---------|-------------|
| WFM     | Wideband (broadcast) FM audio, mono, 75 µs de-emphasis |
| AM      | AM audio (envelope detector) |
| NFM     | Narrowband FM audio |
| Pager   | POCSAG + FLEX, auto-detecting whichever is present |

Decoded text lands in the unified **Messages** panel; audio modes play through
the shared audio output. More decoder types are planned — see
[`docs/ADDING_A_DECODER.md`](docs/ADDING_A_DECODER.md) for the plugin interface.

### Planned

- ADS-B (1090 MHz) and ERT/SCM utility meters — these are wideband, full-stream
  modes and will arrive as a separate decoder family.

## Building

SignalScope builds with the **MSYS2 / MinGW-w64** toolchain using CMake + Ninja.
See [`COMPILE.md`](COMPILE.md) for full instructions, including optional Airspy
and LibreSDR (UHD) setup.

```sh
cmake -S . -B build -G Ninja
ninja -C build
```

## Credits

- **multimon-ng** — Elias Oenal, Thomas Sailer (FLEX)
- **Dear ImGui** / **ImPlot** — Omar Cornut, Evan Pezent
- **miniaudio** — David Reid
- Created by Sarah Rose
