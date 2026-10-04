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

## Installation

Copy the `components/fft_analyzer` folder into your ESPHome config
directory and reference it as a local external component:

### Local
```yaml
external_components:
  - source:
      type: local
      path: components
    components: [fft_analyzer]
```

### Git
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

Good while you're still tuning `noise_floor`/`decay`/gain and want to
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
  overlap: false               # true | false, default false
  noise_floor: 0.0             # >= 0.0, default 0.0 (gate disabled)

  bar_count: 5                 # 0 - 128, default 0 (visualizer bars disabled)
  bar_low_frequency: 50Hz
  bar_high_frequency: 18000Hz
```

| Option               | Type    | Default  | Description |
|-----------------------|---------|----------|-------------|
| `microphone`          | id      | required | The `microphone` component to pull audio from. Expected sample rate is 16 kHz. |
| `fft_size`             | int     | `512`    | FFT window size in samples. Must be a power of two (`256`, `512`, or `1024`). Larger = finer frequency resolution (more Hz/bin), but updates less often and costs more CPU per frame. |
| `decay`                | float   | `0.85`   | Peak-hold + decay smoothing factor, `0.0`-`0.99`. A band/bar jumps immediately to a new, higher value ("peak hold"), but decays towards a new, lower value by this factor instead of snapping to it. Closer to `1.0` = slower, smoother fall-off; closer to `0.0` = near-instant tracking of the raw signal. |
| `overlap`              | bool    | `false`  | `false`: each FFT window is computed from a completely fresh set of samples (simpler, less CPU, slightly choppier animation). `true`: consecutive windows share 50% of their samples (standard overlapping-window technique), roughly doubling the effective update rate and producing visibly smoother animation, at the cost of running the FFT about twice as often. Turn this on if your chip has CPU headroom to spare. |
| `noise_floor`          | float   | `0.0`    | Noise gate. Any band/bar whose raw magnitude is below this value is clamped to `0` *before* peak-hold/decay smoothing, which stops microphone self-noise from showing up as low-level "flicker" on quiet bands. `0.0` disables the gate entirely. See "Tuning the noise gate" below. |
| `bar_count`            | int     | `0`      | Number of generic, log-spaced visualizer bars for an equalizer display. `0` disables them. Not exposed as HA sensors — read with `id(<fft_id>).get_bar(i)` from a display lambda. |
| `bar_low_frequency`    | frequency | `50Hz`  | Lower edge of the frequency range spanned by the bars. |
| `bar_high_frequency`   | frequency | `18000Hz` | Upper edge of the frequency range spanned by the bars. Bars are distributed **logarithmically** between the low and high frequency (more resolution at low frequencies, matching human hearing and the layout of a typical hardware equalizer). |

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

This is useful for tuning (see "Tuning the noise gate" below) but, since
it logs on every single FFT frame, it is fairly noisy and will dominate
your log output while left on. To turn it off, either:

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

The `dump_config()` summary (FFT size, decay, overlap, noise floor,
visualizer bar settings) is logged once at `CONFIG` level on every boot
regardless of this setting — only the per-frame `FFT ...=` line is
`DEBUG`-gated.

## Tuning the noise gate

There's no universal `noise_floor` value — it depends on your
microphone, its gain setting, and ambient noise. To find a good value:

1. Set `logger: level: DEBUG` and leave `noise_floor: 0.0`.
2. In a quiet room, watch the `FFT 60=... 120=... ...` log line for a
   few seconds and note the typical magnitude of the noise floor on your
   quietest bands.
3. Set `noise_floor` to a bit above that value, re-flash, and check that
   quiet-room flicker is gone but real low-level sounds still register.

## Concurrency model

Audio is delivered asynchronously via the microphone's data callback,
which ESPHome's `microphone` implementations invoke from a dedicated
FreeRTOS task — typically running on a **different CPU core** than the
main ESPHome loop task on dual-core chips (ESP32, ESP32-S3, ...). That
means the producer (`process_audio_()`) and the consumer
(`loop()`/`calculate_fft_()`) can genuinely run at the same time on two
different cores.

All access to the shared sample buffer and the "window ready" flag is
guarded by a FreeRTOS mutex. If you fork/modify this component:

- Every early exit from a mutex-locked loop must still reach
  `xSemaphoreGive()`. Using `return` instead of `break` inside the locked
  loop in `process_audio_()` leaves the mutex permanently held and
  freezes the whole device within seconds — this exact bug shipped in an
  earlier version of this component.
- `calculate_fft_()` reads the sample buffer **without** holding the
  mutex. That's only safe because, by the time it runs, the "window
  ready" flag is already `true`, and the producer checks that flag
  (under the mutex) before it will touch the buffer again. Don't break
  that invariant.

See the comments in `fft_analyzer.h`/`fft_analyzer.cpp` for the full
details.

## License

Use, modify, and redistribute freely.
