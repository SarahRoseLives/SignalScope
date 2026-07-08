# Adding a decoder

SignalScope has two decoder families. Pick the one that matches the signal.

## 1. Channel decoders (narrowband)

For anything that fits inside the shared front-end's per-channel DDC (audio
modes, pagers, data channels up to a few tens of kHz). These are placed by
Ctrl+clicking the spectrum and are auto-managed by `DecoderManager` (sub-band
grouping, worker threads, retune, audio routing).

**To add one you write ONE file** in `src/decode/channel/` and register it. No
edits to `Decoder`, `DecoderManager`, or the GUI.

### Steps

1. Implement `IChannelDecoder` (see `channel/ichannel_decoder.h`):

   ```cpp
   #include "decode/channel/channel_registry.h"
   #include "decode/channel/message_bus.h"

   namespace {
   class MyDecoder : public IChannelDecoder {
   public:
       explicit MyDecoder(const ChannelContext& ctx)
           : bus_(ctx.bus), chan_(ctx.channelId), freqMHz_(ctx.freqHz / 1e6),
             rate_(ctx.sampleRate) {}

       void process(const double* iq, int n) override {
           // iq is interleaved (I,Q) doubles at rate_ (your ddcRate).
           // On a decode, post to the bus:
           //   DecodedRecord r; r.timeSec=...; r.channelId=chan_;
           //   r.freqMHz=freqMHz_; r.source="MYPROTO"; r.text="...";
           //   r.fields["key"]="value"; bus_->post(r);
       }
       bool locked() const override { return locked_; }
   private:
       MessageBus* bus_; int chan_; double freqMHz_, rate_; bool locked_ = false;
   };
   } // namespace
   ```

2. Register it at the bottom of the file:

   ```cpp
   REGISTER_CHANNEL_DECODER(ChannelDecoderInfo{
       /*typeId*/ 5,                 // unique, stable, persisted in config
       "My Protocol",               // combo label
       "MYP",                       // table short label
       /*ddcRate*/ 48000.0,         // IF rate feeding process()
       /*ddcBandwidth*/ 12500.0,    // DDC passband (double-sided)
       /*weight*/ 3,                // CPU load hint for worker balancing
       /*isAudio*/ false,           // true => routes to the AudioSink
       /*dedicatedSubband*/ false,  // true => own wideband sub-band (e.g. WFM)
       [](const ChannelContext& c) { return std::make_unique<MyDecoder>(c); }});
   ```

3. Add the `.cpp` to `CMakeLists.txt` under the `SignalScope` sources
   (`src/decode/channel/*.cpp`), reconfigure, build. It appears in the
   "Decode type" combo automatically and its output shows in the Messages panel.

### Notes
- **Type ids are stable** — never renumber existing ones (they're saved in
  `signalscope.ini`). See `ChannelType` in `channel_registry.h`.
- **Audio decoders** override `isAudio()/audioRate()/setAudioActive()/audioActive()`
  and push to `ctx.audio` (the shared `AudioSink`). Only one plays at a time; the
  manager handles selection and auto-play on drop.
- **Wideband channels** (like WFM) set `dedicatedSubband=true`: they get their
  own sub-band whose front-end re-centres on retune for real selectivity.
- Registration uses a static initializer. This works because the decoder `.cpp`
  files are compiled directly into the executable (not a static library that
  could drop unreferenced objects).

## 2. Wideband decoders (full-stream) — future

Modes like ADS-B (1090 MHz, ~2.4 Msps) and ERT/SCM (915 MHz, ~2 Msps) do not
fit the narrowband channel model: they need their own tuning and sample rate and
consume the raw SDR stream directly. These will be a separate registry
(`IWidebandDecoder`) selected as a *source mode* rather than a spectrum channel.
Not implemented yet.
