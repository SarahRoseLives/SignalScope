# Building SignalScope

## Linux

### 1. Install dependencies (Debian/Ubuntu)

```bash
sudo apt install \
  build-essential cmake ninja-build pkg-config \
  libglfw3-dev librtlsdr-dev libhackrf-dev libusb-1.0-0-dev \
  libzstd-dev zlib1g-dev libgl-dev
```

Optional SDR backends:
```bash
sudo apt install libairspy-dev        # Airspy support
sudo apt install libuhd-dev libboost-dev  # LibreSDR / USRP B210
sudo apt install fftw3-dev libao-dev     # for libnrsc5 (see optional section below)
```

### 2. Build

```bash
cmake -S . -B build -G Ninja
ninja -C build
./build/SignalScope
```

### Optional: SDRplay support (Linux)

SignalScope drives the SDRplay RSP family through **SoapySDR** using the
**SoapySDRPlay3** module and the SDRplay API 3 service. Two pieces are needed;
plain `apt install` / `pacman -S` packages do not cover the second one.

1. **SDRplay API 3.15+ and its service.** Download the Linux installer from
   <https://www.sdrplay.com/api/> (the `libsdrplay` AUR package on Arch works
   too). Make sure the service is enabled and running:

   ```bash
   sudo systemctl enable --now sdrplay
   ```

   The driver (`libsdrplay_api.so`) is not redistributable, so it is not bundled.

2. **The SoapySDRPlay3 module**, which provides SoapySDR's `sdrplay` factory.
   Most distros do not package it, so build it against the installed SDRplay API
   headers:

   ```bash
   git clone --depth 1 https://github.com/pothosware/SoapySDRPlay3.git /tmp/SoapySDRPlay3
   cmake -S /tmp/SoapySDRPlay3 -B /tmp/SoapySDRPlay3/build -G Ninja
   ninja -C /tmp/SoapySDRPlay3/build
   sudo cmake --install /tmp/SoapySDRPlay3/build
   ```

   This installs `libsdrPlaySupport.so` into
   `/usr/local/lib/SoapySDR/modules0.8/`, one of SoapySDR's module search paths.

Verify detection before running SignalScope:

```bash
SoapySDRUtil --info                  # should list: Available factories... sdrplay
SoapySDRUtil --find="driver=sdrplay" # should print your RSP (serial, label)
```

CMake reports whether the module was found (`-- SDRplay driver module found: ...`)
and warns if it is missing. If you installed SoapySDRPlay3 somewhere
non-standard, point CMake at it with
`-DSOAPY_SDRPLAY_MODULE=/path/to/libsdrPlaySupport.so`.

---

## Windows

SignalScope is built with the **MSYS2 / MinGW-w64** toolchain (GCC 15.x) using
**CMake** and **Ninja**. The build is reproducible from a clean MSYS2 install.

> Important: GCC must be invoked from inside the **MINGW64** environment. If you
> run `g++.exe` directly from PowerShell/cmd without that environment set up,
> `cc1plus` dies silently with exit code 1 and no diagnostics. Always build from
> the **MSYS2 MINGW64** shell (or set `MSYSTEM=MINGW64` before launching bash).

## 1. Install MSYS2

Download and install from <https://www.msys2.org>, then open the
**"MSYS2 MINGW64"** shell (not the plain "MSYS2 MSYS" shell) from the Start menu.

Update the package database once:

```bash
pacman -Syu
# close and reopen the MINGW64 shell if it asks you to, then:
pacman -Su
```

## 2. Install build tools and dependencies

```bash
pacman -S --needed \
  mingw-w64-x86_64-toolchain \
  mingw-w64-x86_64-cmake \
  mingw-w64-x86_64-ninja \
  mingw-w64-x86_64-pkgconf \
  mingw-w64-x86_64-glfw \
  mingw-w64-x86_64-rtl-sdr \
  mingw-w64-x86_64-hackrf \
  mingw-w64-x86_64-libusb \
  mingw-w64-x86_64-zstd \
  mingw-w64-x86_64-ogg \
  mingw-w64-x86_64-vorbis
```

These provide: GCC/G++, CMake, Ninja, pkg-config, GLFW (windowing), librtlsdr +
libusb (RTL-SDR), HackRF, and zstd (SDR++ server compression). OpenGL and zlib
ship with the toolchain. libogg + libvorbis provide OGG Vorbis voice recording.

Dear ImGui, ImPlot, the JAERO DSP, mbelib, libacars, and miniaudio
are vendored in `third_party/` and need no separate install.

### Optional: Airspy support

```bash
pacman -S --needed mingw-w64-x86_64-libairspy
```

Airspy headers are vendored in `third_party/airspy/`. The build automatically
enables Airspy (`HAS_AIRSPY=1`) when libairspy is found by CMake.

### Optional: SDRplay support

SignalScope drives the SDRplay RSP family through **SoapySDR** using the
**SoapySDRPlay3** module and the SDRplay API 3 service. A matched SoapySDR SDK
(headers, import library, `SoapySDR.dll`, and the `sdrPlaySupport` module) is
vendored in `third_party/pothos/`, so no separate SoapySDR install is required to
build. CMake enables it automatically (`-- SDRplay enabled via SoapySDR: ...`).

Runtime requirements:

- Install the [SDRplay API and service](https://www.sdrplay.com/api/) (3.15 or
  newer) from SDRplay. The driver `sdrplay_api.dll` is not redistributable, so it
  is not bundled.
- The build copies `SoapySDR.dll` next to the exe and the SoapySDRPlay3 module
  into `build/soapy/`; the app points `SOAPY_SDR_ROOT` there at runtime, so it
  works standalone once the SDRplay API service is installed.

RTL-TCP (`rtl_tcp` network source) has no extra dependencies and is always built.

### Optional: HD Radio (NRSC-5) support

SignalScope uses **libnrsc5** to decode HD Radio digital subcarriers on FM
broadcast stations. The decoder shows station info, track metadata, and available
audio programs. A vendored header is included in `third_party/nrsc5/`; the
library must be built from source.

Install build dependencies (FFTW, libao):

```bash
pacman -S --needed \
  mingw-w64-x86_64-fftw \
  mingw-w64-x86_64-libao
```

Build and install libnrsc5:

```bash
git clone --depth 1 https://github.com/theori-io/nrsc5.git /tmp/nrsc5
cd /tmp/nrsc5
cmake -S . -B build -G Ninja
ninja -C build
cp build/src/libnrsc5.dll.a /mingw64/lib/
cp build/src/libnrsc5.dll /mingw64/bin/
```

The build enables NRSC-5 (`HAS_NRSC5=1`) automatically when CMake finds `libnrsc5`
(`-- libnrsc5 link library found: ...`). Two runtime DLLs are needed next to the
exe: `libnrsc5.dll` (copied automatically by the post-build step) and
`libfftw3f-3.dll` (from `mingw-w64-x86_64-fftw`; copy it manually or install the
package).

### Optional: LibreSDR / USRP B210 support

SignalScope drives the LibreSDR (a USRP B210 clone) through **UHD**, which loads
our custom FPGA bitstream (`libresdr_b210.bin`) onto the device at open time.

Install UHD and its Boost dependency (headers *and* libs):

```bash
pacman -S --needed \
  mingw-w64-x86_64-libuhd \
  mingw-w64-x86_64-boost
```

> `mingw-w64-x86_64-boost` (not just `-boost-libs`) is required — UHD's headers
> need `boost/config.hpp` at compile time. CMake also strips the stale
> `-lboost_system` from UHD's `uhd.pc`, since Boost >= 1.69 makes Boost.System
> header-only and MSYS2's Boost ships no `libboost_system`.

The build enables LibreSDR (`HAS_LIBRESDR=1`) automatically when CMake finds UHD
(`-- UHD found: <version>`).

Three extra things are needed on Windows before the device is detected — without
them the app reports **"No LibreSDR/B210 device found"** even though it is plugged
in:

1. **WinUSB driver.** The B210 talks to UHD via libusb, which needs the device
   bound to **WinUSB**. Use [Zadig](https://zadig.akeo.ie): select the device
   (shows up as *WestBridge*, USB ID `2500:0020`), choose **WinUSB** as the
   target driver, and click *Install/Replace Driver*. Unplug and replug after.

2. **UHD firmware/FPGA images.** UHD needs `usrp_b200_fw.hex` (the FX3 USB
   firmware) to bring the device up. Download the images package once:

   ```bash
   pacman -S --needed mingw-w64-x86_64-python-requests
   python /mingw64/lib/uhd/utils/uhd_images_downloader.py -t b2
   ```

   This installs the images into `/mingw64/share/uhd/images`. The build copies
   them into `build/uhd-images/` next to the exe, and the app points
   `UHD_IMAGES_DIR` there at runtime (see `ensureUhdImagesDir()` in
   `src/sdr/libresdr_source.cpp`) so it works standalone. If UHD is installed
   elsewhere, vendor the images into `third_party/uhd-images/` and CMake will use
   those instead.

3. **USB 3 port.** Plug into a USB 3 (blue) port for full sample-rate bandwidth;
   USB 2 works but is rate-limited.

Verify detection from the MINGW64 shell before running the app:

```bash
uhd_find_devices --args type=b200
```

It should print the device (serial, `product: B210`). SignalScope passes
`fpga=libresdr_b210.bin` to UHD at open, so the log shows
`Loading FPGA image: ...libresdr_b210.bin` when you select the LibreSDR source.

## 3. Configure and build

From the **MINGW64** shell, in the project root:

```bash
cmake -S . -B build -G Ninja
ninja -C build
```

The executable and the runtime DLLs it needs are placed in `build/`:

```
build/SignalScope.exe
build/libgcc_s_seh-1.dll, libwinpthread-1.dll, libstdc++-6.dll,
      glfw3.dll, librtlsdr.dll, libhackrf.dll, libusb-1.0.dll,
      libzstd.dll, zlib1.dll, libogg-0.dll, libvorbis-0.dll,
      libvorbisenc-2.dll
```
(plus `build/libairspy.dll` when Airspy support is enabled, and `build/SoapySDR.dll`
plus a `build/soapy/` driver tree when SDRplay support is enabled)

When LibreSDR support is enabled, the build additionally places next to the exe:

```
build/libuhd.dll, libboost_filesystem-mt.dll, libboost_serialization-mt.dll,
      libboost_thread-mt.dll, libpython3.12.dll
build/libresdr_b210.bin          (our custom FPGA bitstream)
build/uhd-images/                (UHD firmware + stock FPGA images)
```

The DLLs are copied next to the `.exe` automatically (POST_BUILD step), so it
runs standalone from a double-click or from PyCharm without MSYS2 on `PATH`.

Run it:

```bash
./build/SignalScope.exe
```

## 4. Building from PowerShell (optional)

If you prefer to drive the build from PowerShell, you must enter the MINGW64
environment first. For example:

```powershell
$env:MSYSTEM = "MINGW64"
$env:CHERE_INVOKING = "1"
& C:\msys64\usr\bin\bash.exe -lc "cd /c/path/to/SignalScope && ninja -C build"
```

The `-l` (login) shell with `MSYSTEM=MINGW64` sources `/etc/profile`, which puts
`/mingw64/bin` on `PATH` so `cc1plus` can find its runtime. `CHERE_INVOKING=1`
keeps the current directory.

## Notes / troubleshooting

- **First build is slow (~1 min on `implot_items.cpp`).** ImPlot's
  `implot_items.cpp` is extremely template-heavy; at `-O2` it takes ~5 minutes
  to compile. CMakeLists.txt overrides it to `-O1` (last `-O` flag wins) so it
  builds in about a minute. This is intentional, not a hang.
- **`cc1plus` exits 1 with no output.** You are not in the MINGW64 environment.
  Build from the MSYS2 MINGW64 shell (see the warning above).
- **Link fails with "Access is denied" on `SignalScope.exe`.** A previous instance
  is still running and is locking the file. Close it, then rebuild.
- **`fatal error: boost/config.hpp: No such file`.** Only `-boost-libs` is
  installed. Install the full `mingw-w64-x86_64-boost` (headers).
- **`cannot find -lboost_system`.** UHD's `uhd.pc` lists it but Boost >= 1.69
  makes Boost.System header-only. CMakeLists.txt already strips it from
  `UHD_LIBRARIES`; re-run `cmake -S . -B build` if you see this.
- **"No LibreSDR/B210 device found" but it is plugged in.** In order, check:
  (1) the WinUSB driver is bound via Zadig (`uhd_find_devices --args type=b200`
  should list it); (2) the UHD images are present — the log line
  `Could not find path for image: usrp_b200_fw.hex` means you skipped the
  `uhd_images_downloader.py` step; (3) `build/uhd-images/usrp_b200_fw.hex` exists
  next to the exe (re-run `ninja -C build` to copy it). The app logs the resolved
  `UHD_IMAGES_DIR=...` line in `build/log.txt` on device select.
- **Incremental builds.** Re-run `ninja -C build`. Only changed files recompile.
  After editing `CMakeLists.txt`, re-run `cmake -S . -B build` first.
- **Clean rebuild.** Delete the `build/` directory and re-run the configure +
  build steps. (Avoid `--clean-first`; it wipes the vendored objects and forces
  the slow `implot_items.cpp` recompile.)
```
