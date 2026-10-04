#pragma once

#include "esphome/core/component.h"
#include "esphome/components/microphone/microphone.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <vector>

namespace esphome {
namespace fft_analyzer {

/// Number of fixed, named frequency bands that are always computed and
/// logged at DEBUG level (60 Hz .. 16 kHz). These exist purely for
/// diagnostics via the log - they are NOT exposed as Home Assistant
/// sensors, to avoid creating entities nobody asked for.
static constexpr size_t NAMED_BAND_COUNT = 9;

/// FFTAnalyzer performs a real-time FFT over audio samples pulled from
/// an ESPHome `microphone` component and exposes the resulting
/// spectrum in two forms:
///
///  1. A fixed set of 9 named bands (60 Hz, 120 Hz, 250 Hz, 500 Hz,
///     1 kHz, 2 kHz, 4 kHz, 8 kHz, 16 kHz), logged at DEBUG level for
///     diagnostics only.
///  2. A configurable number of logarithmically-spaced "bars"
///     (`bar_count`), intended for driving a spectrum/equalizer
///     visualization from a `display:` lambda via `get_bar(i)`.
///
/// Neither form is published as a Home Assistant sensor. If you need
/// the values in Home Assistant, read them from a display lambda or
/// add your own `sensor:` wrapper on top of this component.
///
/// ## Concurrency model
///
/// Audio is delivered asynchronously via the microphone's data
/// callback, which ESPHome's `microphone` implementations invoke from
/// a dedicated FreeRTOS task - typically running on a *different* CPU
/// core than the main ESPHome loop task on dual-core chips (ESP32,
/// ESP32-S3, ...). That means `process_audio_()` (producer) and
/// `loop()`/`calculate_fft_()` (consumer) can genuinely run at the
/// same time on two different cores.
///
/// All access to the shared sample buffer (`samples_`) and the
/// `new_data_` flag is therefore guarded by a FreeRTOS mutex
/// (`buffer_mutex_`). `calculate_fft_()` itself reads `samples_`
/// *without* holding the mutex - this is safe only because, by the
/// time it runs, `new_data_` is already `true`, and the producer
/// checks that flag (under the same mutex) before touching
/// `samples_` again. If you modify this flow, keep that invariant: the
/// producer must never write to `samples_` while `new_data_` is true.
///
/// A previous version of this component used `return` instead of
/// `break` inside the producer's locked loop, which left the mutex
/// permanently held and froze the whole device within seconds. If you
/// are refactoring this file, double-check every early exit from a
/// locked section actually reaches `xSemaphoreGive()`.
class FFTAnalyzer : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;

  // ---- Configuration setters (called from __init__.py's to_code) ----

  void set_microphone(microphone::Microphone *mic) { this->mic_ = mic; }

  /// FFT window size in samples. Must be a power of two (256/512/1024).
  /// Larger windows give finer frequency resolution (more Hz/bin
  /// precision) but update less often and cost more CPU per frame.
  void set_fft_size(size_t size) { this->fft_size_ = size; }

  /// Peak-hold + decay smoothing factor, 0.0-0.99. On every frame, a
  /// band either jumps up immediately to a new, higher value ("peak
  /// hold") or decays towards the new value by this factor if the new
  /// value is lower. Closer to 1.0 = slower, smoother fall-off after a
  /// peak; closer to 0.0 = near-instant tracking of the raw signal.
  void set_decay(float decay) { this->decay_ = decay; }

  /// Window overlap mode.
  ///
  /// - `false` (default): each FFT window is computed from a
  ///   completely fresh set of samples. Simpler, lower CPU load,
  ///   slightly choppier-looking animation.
  /// - `true`: consecutive windows share 50% of their samples (a
  ///   standard overlapping-window technique). This roughly doubles
  ///   the effective update rate and produces visibly smoother
  ///   animation, at the cost of computing the FFT about twice as
  ///   often. Recommended if your chip has CPU headroom to spare.
  void set_overlap(bool overlap) { this->overlap_ = overlap; }

  /// Noise gate threshold. Any band/bar whose raw magnitude falls
  /// below this value is clamped to 0 *before* peak-hold/decay
  /// smoothing is applied, which stops microphone self-noise from
  /// showing up as low-level "flicker" on quiet bands.
  ///
  /// 0.0 (default) disables the gate entirely - every value, however
  /// small, is passed through unmodified.
  ///
  /// Magnitude is on the same unitless scale as the values printed in
  /// the DEBUG log (`FFT 60=... 120=...`); watch that log in a quiet
  /// room to find a sensible threshold for your microphone/gain
  /// combination before setting this.
  void set_noise_floor(float floor) { this->noise_floor_ = floor; }

  /// Number of generic, log-spaced visualizer bars (0 = disabled,
  /// default). These are NOT Home Assistant sensors - read them from a
  /// display lambda via `id(<fft_id>).get_bar(i)`.
  void set_bar_count(size_t count) {
    this->bar_count_ = count;
    this->bars_.resize(count, 0.0f);
    this->bar_edges_low_.resize(count, 0);
    this->bar_edges_high_.resize(count, 0);
    this->bar_edges_ready_ = false;
  }

  /// Frequency range spanned by the `bar_count` bars. Bars are
  /// distributed logarithmically between `low_hz` and `high_hz` (more
  /// resolution at low frequencies, matching human hearing and the
  /// layout of a typical hardware equalizer).
  void set_bar_freq_range(float low_hz, float high_hz) {
    this->bar_low_hz_ = low_hz;
    this->bar_high_hz_ = high_hz;
  }

  // ---- Runtime accessors for display lambdas ----

  /// Number of configured visualizer bars (`bar_count` from YAML).
  size_t get_bar_count() const { return this->bar_count_; }

  /// Smoothed (peak-hold + decay) magnitude of visualizer bar `index`,
  /// or 0.0 if `index` is out of range. Safe to call every frame from
  /// a display lambda.
  float get_bar(size_t index) const {
    if (index >= this->bars_.size()) return 0.0f;
    return this->bars_[index];
  }

 protected:
  void process_audio_(const std::vector<uint8_t> &data);
  void calculate_fft_();
  void compute_bar_edges_();

  /// Guards all access to `samples_` and `new_data_`. See the class-
  /// level "Concurrency model" comment above before touching this.
  SemaphoreHandle_t buffer_mutex_{nullptr};

  microphone::Microphone *mic_{nullptr};

  size_t fft_size_{512};
  bool overlap_{false};
  float noise_floor_{0.0f};

  /// Peak-hold + decay smoothing factor, set via `set_decay()`. See the
  /// documentation on that setter for how it's used.
  float decay_{0.85f};

  /// Raw PCM samples awaiting FFT, normalized to [-1.0, 1.0]. Shared
  /// between the microphone task (producer) and the main loop task
  /// (consumer) - always access under `buffer_mutex_`.
  std::vector<float> samples_;

  /// Precomputed Hann window, same size as `fft_size_`.
  std::vector<float> window_;

  /// FFT working buffers (real/imaginary), reused every frame.
  std::vector<float> real_;
  std::vector<float> imag_;

  /// Set by the microphone task once `samples_` holds a full window;
  /// cleared by the main loop task once that window has been consumed.
  /// Always access under `buffer_mutex_`, with the one exception noted
  /// in the class-level comment.
  bool new_data_{false};

  /// Raw (unsmoothed) magnitude of each of the 9 fixed named bands,
  /// from the most recently computed FFT frame.
  float bands_[NAMED_BAND_COUNT]{};

  /// Peak-hold + decay smoothed magnitude of each named band. This is
  /// what gets printed in the DEBUG log line.
  float smoothed_[NAMED_BAND_COUNT]{};

  // ---- Generic log-spaced visualizer bars ----

  size_t bar_count_{0};
  float bar_low_hz_{50.0f};
  float bar_high_hz_{18000.0f};
  /// True once `compute_bar_edges_()` has run (requires `fft_size_` to
  /// be known, so it happens at the end of `setup()`, not in
  /// `set_bar_count()`).
  bool bar_edges_ready_{false};

  /// Peak-hold + decay smoothed magnitude of each visualizer bar.
  std::vector<float> bars_;
  /// Lowest/highest FFT bin index covered by each visualizer bar,
  /// precomputed once in `compute_bar_edges_()`.
  std::vector<int> bar_edges_low_;
  std::vector<int> bar_edges_high_;
};

}  // namespace fft_analyzer
}  // namespace esphome
