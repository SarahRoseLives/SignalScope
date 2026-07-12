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
| HD Radio | NRSC-5 digital FM: station info, track metadata (ID3), multi-program audio, BER/MER signal quality, station location |
| OOB EPG | SCTE 55-2 cable out-of-band EPG: channel lineup (callsign → virtual channel #), service display names, MAC control-channel provisioning data (VCI 0x0021), OCAP/tru2way host configuration (VCI 0x0FA2), plus readable system strings from the DSM-CC object carousel |

Decoded text lands in the unified **Pager** panel; audio modes play through
the shared audio output. The OOB EPG decoder has its own dedicated **EPG** tab
with collapsible sections for channel lineup, service names, MAC control messages,
host config, and raw system strings. The HD Radio decoder has a dedicated **HD Radio**
tab showing station info, now-playing track, program selector, signal quality,
and technical details. More decoder types are planned — see
[`docs/ADDING_A_DECODER.md`](docs/ADDING_A_DECODER.md) for the plugin interface.

### Planned

- ADS-B (1090 MHz) and ERT/SCM utility meters — these are wideband, full-stream
  modes and will arrive as a separate decoder family.

## OOB EPG Cable Decoder

The SCTE 55-2 (DAVIC Mode B) out-of-band decoder processes the cable forward data
channel (772 ksym/s QPSK, 1.544 Mb/s). It implements the full protocol stack:

```
IQ → QPSK Demod → Diff Decode → Derandomize → SL-ESF Frame Sync
   → RS(55,53) FEC → ATM Cells → AAL5 → IP/UDP → DSM-CC Carousel
   → EPG Extraction
```

**Extracted data types:**
- **Channel Lineup** — callsign → virtual channel number (from DAVIC channel map, tag `0x0B`)
- **Service Names** — friendly multi-word display names (e.g. "Paramount+ with SHOWTIME")
- **MAC Control** — provisioning/service channel frequencies, upstream rates, power levels (VCI 0x0021)
- **Host Config** — OCAP bootstrap: in-band QAM frequency/program, CVS/firmware URLs, `-D` flags (VCI 0x0FA2)
- **System Strings** — firmware identifiers, CableCARD certs, filenames, UI messages extracted from ATM payloads
- **BIOP Objects** — carousel file inventory (service gateway, configuration files, logo resources)

> **Note:** Requires a cable TV provider that broadcasts SCTE 55-2 OOB forward data
> (tested on Spectrum/Charter). Place the decoder at the OOB frequency (typically
> 70–130 MHz). Use a sample rate ≥ 2 Msps for reliable decoding. The Airspy is
> recommended for best sensitivity.

## HD Radio (NRSC-5) Decoder

Decodes HD Radio digital subcarriers on FM broadcast stations via **libnrsc5**.
Place the decoder at the center frequency of any FM station (88–108 MHz) with
digital HD Radio sidebands.

**Features:**
- Multi-program audio with program selector
- Station info: call sign, slogan, facility ID, transmitter location
- Now Playing track metadata (title, artist, album, genre) via ID3
- Signal quality: MER (modulation error ratio), BER (bit error rate)
- Codec details (HE-AAC mode, blend control, gain, latency)
- Program type per channel (Rock, News, Jazz, etc.)
- Local time with DST info from the station
- Emergency alerts

**Build requirement:** See [`COMPILE.md`](COMPILE.md) for libnrsc5 build instructions.
The library must be built from source with FAAD2 (AAC audio codec) enabled.
Requires a sample rate ≥ 6 Msps for reliable digital lock — the Airspy or HackRF
is recommended.

## Building

SignalScope builds with the **MSYS2 / MinGW-w64** toolchain using CMake + Ninja.
See [`COMPILE.md`](COMPILE.md) for full instructions, including optional Airspy
and LibreSDR (UHD) setup.

```sh
cmake -S . -B build -G Ninja
ninja -C build
```

## Credits

- **CableSniffer** — OOB EPG protocol reverse-engineering (SCTE 55-2)
- **multimon-ng** — Elias Oenal, Thomas Sailer (FLEX)
- **Dear ImGui** / **ImPlot** — Omar Cornut, Evan Pezent
- **miniaudio** — David Reid
- **zlib** — Jean-loup Gailly, Mark Adler
- Created by Sarah Rose
