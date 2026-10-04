# fft_analyzer — ESPHome real-time FFT audio analyzer

A custom ESPHome `external_components` that reads audio from an ESPHome
`microphone` source, runs a real-time FFT over it, and exposes the
resulting spectrum for driving an equalizer / spectrum visualization on a
`display:` (originally built and tested against a HUB75 LED matrix, but
nothing in the component is display-specific).

It does **not** create any Home Assistant sensors/entities. All output is
read either from the DEBUG log (9 fixed named bands, for diagnostics) or
from a `display:` lambda via `id(<fft_id>).get_bar(i)` (a configurable
number of generic, log-spaced visualizer bars). If you need the values in
Home Assistant, wrap them in your own `sensor:` using a `lambda:` source.

## Installation local

Copy the `components/fft_analyzer` folder into your ESPHome config
directory and reference it as a local external component:

```yaml
external_components:
  - source:
      type: local
      path: components
    components: [fft_analyzer]
```

## Installation Git

```yaml
external_components:
  - source: github://cosmicDustOfLightLength/esphome-fft_analyzer@master
    components: [fft_analyzer]
```

## Minimal configuration

```yaml
microphone:
  - platform: i2s_audio
    id: mic_id
    # ... your microphone config ...

fft_analyzer:
  id: fft
  microphone: mic_id
```

⚠️ **This alone is not enough.** ESPHome's `microphone` components don't
stream audio by default — they sit idle until something calls
`microphone.capture`. Until that happens, `fft_analyzer` never receives
any data: no crash, no error, just flat zeros forever. See the next
section for how to start it.

## Starting the microphone

`fft_analyzer` only processes audio that the microphone is actually
capturing. You start/stop that with the `microphone.capture` /
`microphone.stop_capture` actions — there are three common ways to
trigger them:

**1. On boot (always listening)**

```yaml
esphome:
  on_boot:
    priority: -100   # low priority = runs at the very end of the boot sequence
    then:
      - delay: 2s     # give the mic driver/I2S bus time to settle before first capture
      - microphone.capture: mic_id
```

Simplest option, good for a dedicated equalizer that should just always
be running. The `delay: 2s` matters: calling `microphone.capture` too
early in the boot sequence (priority too high / no delay) can crash with
`assert failed: xQueueSemaphoreTake` on some I2S drivers — give the bus
time to initialize first.

**2. A button (one-shot, for testing)**

```yaml
button:
  - platform: template
    name: "Start microphone"
    on_press:
      - microphone.capture: mic_id
```

Good while you're still tuning `decay`/bar range/gain and want to
start/restart capture on demand without rebooting the device. Not
convenient for normal use since there's no persistent on/off state.

**3. A switch (on/off, recommended if you don't want it always running)**

```yaml
switch:
  - platform: template
    name: "Equalizer microphone"
    optimistic: true
    turn_on_action:
      - microphone.capture: mic_id
    turn_off_action:
      - microphone.stop_capture: mic_id
```

Best middle ground: persists its state, is controllable from Home
Assistant, and lets you turn the mic off (saving CPU/power, and avoiding
picking up audio) when the equalizer display isn't needed. **Recommended**
unless you specifically want the equalizer running unconditionally from
boot.

## Full configuration reference

```yaml
fft_analyzer:
  id: fft
  microphone: mic_id         # required, id of an ESPHome `microphone` component

  fft_size: 512               # 256 | 512 | 1024, default 512
  decay: 0.85                 # 0.0 - 0.99, default 0.85

  bar_count: 5                 # 0 - 128, default 0 (visualizer bars disabled)
  bar_low_frequency: 50Hz
  bar_high_frequency: 18000Hz
```

| Option               | Type    | Default  | Description |
|-----------------------|---------|----------|-------------|
| `microphone`          | id      | required | The `microphone` component to pull audio from. The FFT math assumes a 48 kHz sample rate — if your mic runs at a different rate, the named-band and bar frequency ranges below will be off proportionally. |
| `fft_size`             | int     | `512`    | FFT window size in samples. Must be a power of two (`256`, `512`, or `1024`). Larger = finer frequency resolution (more Hz/bin), but updates less often and costs more CPU per frame. |
| `decay`                | float   | `0.85`   | Peak-hold + decay smoothing factor, `0.0`-`0.99`. A band/bar jumps immediately to a new, higher value ("peak hold"), but decays towards a new, lower value by this factor instead of snapping to it. Closer to `1.0` = slower, smoother fall-off; closer to `0.0` = near-instant tracking of the raw signal. |
| `bar_count`            | int     | `0`      | Number of generic, log-spaced visualizer bars for an equalizer display. `0` disables them. Not exposed as HA sensors — read with `id(<fft_id>).get_bar(i)` from a display lambda. |
| `bar_low_frequency`    | frequency | `50Hz`  | Lower edge of the frequency range spanned by the bars. |
| `bar_high_frequency`   | frequency | `18000Hz` | Upper edge of the frequency range spanned by the bars. Bars are distributed **logarithmically** between the low and high frequency (more resolution at low frequencies, matching human hearing and the layout of a typical hardware equalizer). |

There is currently no YAML option for window overlap or a noise gate —
each FFT window starts from a freshly cleared sample buffer, and no
magnitude thresholding is applied before smoothing. Earlier drafts of
this component experimented with both (50% overlapping windows, a
configurable noise floor), but they added complexity that wasn't earning
its keep and were dropped from this release in favor of the simpler,
known-good version documented here. If you want either, see "Extending
this component" below.

## Runtime API (for display lambdas)

```cpp
id(fft).get_bar_count()     // number of configured bars (bar_count from YAML)
id(fft).get_bar(i)          // smoothed magnitude of bar i, or 0.0 if i is out of range
```

Both are safe to call every frame. See `examples/equalizer_peak_hold.yaml`
for a full peak-hold "bouncing ball" equalizer display built on top of
these two calls.

The 9 fixed named bands (60 Hz, 120 Hz, 250 Hz, 500 Hz, 1 kHz, 2 kHz,
4 kHz, 8 kHz, 16 kHz) are logged at `DEBUG` level only — they exist for
diagnostics/tuning and aren't exposed through any other API.

## Logging

Every FFT frame, the component logs the 9 fixed named bands at `DEBUG`
level, one line per frame:

```
[D][fft_analyzer:xxx]: FFT 60=0.012 120=0.034 250=0.021 500=0.045 1k=0.102 2k=0.087 4k=0.033 8k=0.015 16k=0.006
```

This is useful for tuning `decay`/mic gain but, since it logs on every
single FFT frame, it is fairly noisy and will dominate your log output
while left on. To turn it off, either:

- Set the global log level below `DEBUG` (e.g. `INFO`) in your YAML:
  ```yaml
  logger:
    level: INFO
  ```
- Or keep `DEBUG` for everything else and silence just this component:
  ```yaml
  logger:
    level: DEBUG
    logs:
      fft_analyzer: INFO
  ```

The `dump_config()` summary (FFT size, sample rate, decay, visualizer bar
settings) is logged once at `CONFIG` level on every boot regardless of
this setting — only the per-frame `FFT ...=` line is `DEBUG`-gated.

## Concurrency model

Audio is delivered asynchronously via the microphone's data callback,
which ESPHome's `microphone` implementations invoke from a dedicated
FreeRTOS task — typically running on a **different CPU core** than the
main ESPHome loop task on dual-core chips (ESP32, ESP32-S3, ...). That
means the producer (`process_audio_()`, mic task) and the consumer
(`loop()`/`calculate_fft_()`, main task) can genuinely run at the same
time on two different cores.

**This version does not use a mutex** to guard the shared sample buffer.
It relies instead on the `new_data_` flag being set only after a window
fills up, and on `loop()` clearing `new_data_` before `calculate_fft_()`
runs, to keep the producer from writing into the buffer while it's being
read. This is simpler and has worked reliably in practice, but it is not
a textbook-correct lock-free design — there is a narrow theoretical
window where the two tasks could touch `samples_` at the same time. An
earlier draft of this component added a FreeRTOS mutex around all buffer
access to close that window properly; it was pulled from this release
because it didn't resolve the actual problem being chased at the time
and added real complexity for no observed benefit. If you see occasional
corrupted-looking frames (a sudden huge spike or stuck value) and want to
rule this out, reintroducing a mutex around `samples_`/`new_data_` in
`process_audio_()` and `loop()` is the place to start.

## Extending this component

Starting points if you want to pick this component up further:

- **Noise gate**: in `calculate_fft_()`, right after `value`/the bar
  magnitude is computed and before it's written into `smoothed_[]`/
  `bars_[]`, clamp it to `0.0f` when it's below a threshold. Expose the
  threshold as a new `set_noise_floor()` setter and a `noise_floor` YAML
  option (`cv.Optional("noise_floor", default=0.0): cv.float_range(min=0.0)`).
- **Window overlap**: in `loop()`, instead of `this->samples_.clear()`,
  keep the second half of the buffer with an in-place `memmove()` +
  `resize(fft_size_ / 2)` (shrinking never reallocates), so the next
  window reuses half its samples. Gate it behind a `set_overlap()`
  setter / `overlap` YAML boolean so it's opt-in.
- **Actual sample rate**: `SAMPLE_RATE` is a hardcoded `48000.0f`
  constant in `fft_analyzer.cpp`. If you use this with a microphone
  running at a different rate, update that constant (or better, make it
  a YAML option) to keep the named-band and bar frequency ranges
  accurate.

## License

Use, modify, and redistribute freely.
