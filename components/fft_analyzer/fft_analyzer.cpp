#include "fft_analyzer.h"

#include "esphome/core/log.h"
#include <cmath>
#include <cstring>

namespace esphome {
namespace fft_analyzer {

static const char *const TAG = "fft_analyzer";

// Center frequencies of the 9 fixed, named diagnostic bands (Hz).
static const float NAMED_BAND_LOW[NAMED_BAND_COUNT] = {
    40, 90, 180, 350, 700, 1400, 2800, 5600, 11200};
static const float NAMED_BAND_HIGH[NAMED_BAND_COUNT] = {
    90, 180, 350, 700, 1400, 2800, 5600, 11200, 20000};
static const float NAMED_BAND_LABEL[NAMED_BAND_COUNT] = {
    60, 120, 250, 500, 1000, 2000, 4000, 8000, 16000};

void FFTAnalyzer::setup() {
  if (this->mic_ == nullptr) {
    ESP_LOGE(TAG, "No microphone configured, aborting setup");
    this->mark_failed();
    return;
  }

  if ((this->fft_size_ & (this->fft_size_ - 1)) != 0) {
    ESP_LOGE(TAG, "fft_size must be a power of two, got %u", this->fft_size_);
    this->mark_failed();
    return;
  }

  this->buffer_mutex_ = xSemaphoreCreateMutex();
  if (this->buffer_mutex_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create buffer mutex, aborting setup");
    this->mark_failed();
    return;
  }

  this->samples_.reserve(this->fft_size_);
  this->window_.resize(this->fft_size_);
  this->real_.resize(this->fft_size_);
  this->imag_.resize(this->fft_size_);

  // Precompute the Hann window once; applying it per-frame is just a
  // multiply, the expensive cosf() call happens only here at boot.
  for (size_t i = 0; i < this->fft_size_; i++) {
    this->window_[i] =
        0.5f * (1.0f - cosf(2.0f * (float) M_PI * i / (this->fft_size_ - 1)));
  }

  this->mic_->add_data_callback([this](const std::vector<uint8_t> &data) {
    this->process_audio_(data);
  });

  if (this->bar_count_ > 0) {
    this->compute_bar_edges_();
  }

  ESP_LOGCONFIG(TAG, "FFT analyzer set up (fft_size=%u)", this->fft_size_);
}

void FFTAnalyzer::compute_bar_edges_() {
  // Microphone sample rate isn't known to this component directly, but all
  // of ESPHome's `microphone` platforms used with this component run at
  // 16 kHz, so bin width = 16000 / fft_size. If you use a different sample
  // rate, the bars will be scaled but still monotonic and log-spaced.
  const float sample_rate = 16000.0f;
  const float bin_hz = sample_rate / (float) this->fft_size_;
  const size_t max_bin = this->fft_size_ / 2;

  const float log_low = log10f(this->bar_low_hz_);
  const float log_high = log10f(this->bar_high_hz_);
  const float log_step = (log_high - log_low) / (float) this->bar_count_;

  for (size_t i = 0; i < this->bar_count_; i++) {
    float f_low = powf(10.0f, log_low + log_step * i);
    float f_high = powf(10.0f, log_low + log_step * (i + 1));

    int bin_low = (int) (f_low / bin_hz);
    int bin_high = (int) (f_high / bin_hz);

    if (bin_low < 1) bin_low = 1;  // skip DC bin
    if (bin_high <= bin_low) bin_high = bin_low + 1;
    if (bin_high > (int) max_bin) bin_high = (int) max_bin;

    this->bar_edges_low_[i] = bin_low;
    this->bar_edges_high_[i] = bin_high;
  }

  this->bar_edges_ready_ = true;
}

void FFTAnalyzer::process_audio_(const std::vector<uint8_t> &data) {
  // Producer side: runs on the microphone's own FreeRTOS task, which on
  // dual-core chips may be pinned to a different core than the main
  // ESPHome loop task. See the "Concurrency model" note in fft_analyzer.h
  // before changing anything in this function.
  if (data.empty() || this->buffer_mutex_ == nullptr) {
    return;
  }

  const size_t sample_count = data.size() / sizeof(int16_t);
  const int16_t *raw = reinterpret_cast<const int16_t *>(data.data());

  xSemaphoreTake(this->buffer_mutex_, portMAX_DELAY);

  for (size_t i = 0; i < sample_count; i++) {
    // The consumer (loop()) hasn't picked up the previous window yet -
    // drop samples until it does, rather than growing the buffer past
    // fft_size_. Using `break` here (NOT `return`!) is essential: this
    // loop runs inside the locked section, and `return` would skip the
    // xSemaphoreGive() below and deadlock the device permanently.
    if (this->new_data_) {
      break;
    }

    this->samples_.push_back(raw[i] / 32768.0f);

    if (this->samples_.size() >= this->fft_size_) {
      this->new_data_ = true;
      // Same reasoning as above: `break`, not `return`.
      break;
    }
  }

  xSemaphoreGive(this->buffer_mutex_);
}

void FFTAnalyzer::loop() {
  if (this->buffer_mutex_ == nullptr) {
    return;
  }

  bool ready;
  xSemaphoreTake(this->buffer_mutex_, portMAX_DELAY);
  ready = this->new_data_;
  xSemaphoreGive(this->buffer_mutex_);

  if (!ready) {
    return;
  }

  // calculate_fft_() reads samples_ without holding the mutex. That is
  // safe ONLY because new_data_ is already true at this point, and the
  // producer (process_audio_()) checks new_data_ under the mutex before
  // it will touch samples_ again - see the class-level comment in the
  // header for the full invariant.
  this->calculate_fft_();

  xSemaphoreTake(this->buffer_mutex_, portMAX_DELAY);

  if (this->overlap_) {
    // Keep the second half of the window as the first half of the next
    // one (50% overlap). This is done in-place with memmove()+resize()
    // rather than allocating a new vector: shrinking a std::vector never
    // reallocates, so there is no per-frame heap churn and nothing gets
    // forced into internal RAM by SPIRAM_MALLOC_ALWAYSINTERNAL.
    const size_t half = this->fft_size_ / 2;
    std::memmove(this->samples_.data(), this->samples_.data() + half,
                 half * sizeof(float));
    this->samples_.resize(half);
  } else {
    // Each window starts from a completely empty buffer.
    this->samples_.clear();
  }

  this->new_data_ = false;
  xSemaphoreGive(this->buffer_mutex_);
}

void FFTAnalyzer::calculate_fft_() {
  const size_t n = this->fft_size_;

  // DC removal: subtract the mean so the FFT isn't dominated by a
  // constant offset coming from the microphone/ADC.
  float mean = 0.0f;
  for (size_t i = 0; i < n; i++) {
    mean += this->samples_[i];
  }
  mean /= (float) n;

  for (size_t i = 0; i < n; i++) {
    this->real_[i] = (this->samples_[i] - mean) * this->window_[i];
    this->imag_[i] = 0.0f;
  }

  // In-place bit-reversal permutation.
  for (size_t i = 1, j = 0; i < n; i++) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) {
      j ^= bit;
    }
    j ^= bit;
    if (i < j) {
      std::swap(this->real_[i], this->real_[j]);
      std::swap(this->imag_[i], this->imag_[j]);
    }
  }

  // Iterative radix-2 Cooley-Tukey FFT.
  for (size_t len = 2; len <= n; len <<= 1) {
    float ang = -2.0f * (float) M_PI / (float) len;
    float wr = cosf(ang);
    float wi = sinf(ang);
    for (size_t i = 0; i < n; i += len) {
      float cur_wr = 1.0f, cur_wi = 0.0f;
      for (size_t k = 0; k < len / 2; k++) {
        float ur = this->real_[i + k];
        float ui = this->imag_[i + k];
        float vr = this->real_[i + k + len / 2] * cur_wr -
                    this->imag_[i + k + len / 2] * cur_wi;
        float vi = this->real_[i + k + len / 2] * cur_wi +
                    this->imag_[i + k + len / 2] * cur_wr;

        this->real_[i + k] = ur + vr;
        this->imag_[i + k] = ui + vi;
        this->real_[i + k + len / 2] = ur - vr;
        this->imag_[i + k + len / 2] = ui - vi;

        float next_wr = cur_wr * wr - cur_wi * wi;
        float next_wi = cur_wr * wi + cur_wi * wr;
        cur_wr = next_wr;
        cur_wi = next_wi;
      }
    }
  }

  const float sample_rate = 16000.0f;
  const float bin_hz = sample_rate / (float) n;

  // ---- 9 fixed named bands (diagnostics only, logged at DEBUG) ----
  for (size_t b = 0; b < NAMED_BAND_COUNT; b++) {
    int bin_low = (int) (NAMED_BAND_LOW[b] / bin_hz);
    int bin_high = (int) (NAMED_BAND_HIGH[b] / bin_hz);
    if (bin_low < 1) bin_low = 1;
    if (bin_high <= bin_low) bin_high = bin_low + 1;
    if (bin_high > (int) (n / 2)) bin_high = (int) (n / 2);

    float peak = 0.0f;
    for (int bin = bin_low; bin < bin_high; bin++) {
      float mag = sqrtf(this->real_[bin] * this->real_[bin] +
                         this->imag_[bin] * this->imag_[bin]);
      if (mag > peak) peak = mag;
    }

    // Noise gate: clamp anything below the configured floor to zero
    // *before* it reaches the peak-hold/decay filter, so quiet-room mic
    // self-noise can't "flicker" a band on and off.
    if (peak < this->noise_floor_) {
      peak = 0.0f;
    }

    this->bands_[b] = peak;

    if (peak > this->smoothed_[b]) {
      this->smoothed_[b] = peak;  // instant peak-hold
    } else {
      this->smoothed_[b] =
          this->smoothed_[b] * this->decay_ + peak * (1.0f - this->decay_);
    }
  }

  ESP_LOGD(TAG,
           "FFT 60=%.3f 120=%.3f 250=%.3f 500=%.3f 1k=%.3f 2k=%.3f 4k=%.3f "
           "8k=%.3f 16k=%.3f",
           this->smoothed_[0], this->smoothed_[1], this->smoothed_[2],
           this->smoothed_[3], this->smoothed_[4], this->smoothed_[5],
           this->smoothed_[6], this->smoothed_[7], this->smoothed_[8]);

  // ---- Generic log-spaced visualizer bars ----
  if (this->bar_count_ > 0 && this->bar_edges_ready_) {
    for (size_t i = 0; i < this->bar_count_; i++) {
      int bin_low = this->bar_edges_low_[i];
      int bin_high = this->bar_edges_high_[i];

      float peak = 0.0f;
      for (int bin = bin_low; bin < bin_high; bin++) {
        float mag = sqrtf(this->real_[bin] * this->real_[bin] +
                           this->imag_[bin] * this->imag_[bin]);
        if (mag > peak) peak = mag;
      }

      if (peak < this->noise_floor_) {
        peak = 0.0f;
      }

      if (peak > this->bars_[i]) {
        this->bars_[i] = peak;
      } else {
        this->bars_[i] =
            this->bars_[i] * this->decay_ + peak * (1.0f - this->decay_);
      }
    }
  }
}

void FFTAnalyzer::dump_config() {
  ESP_LOGCONFIG(TAG, "FFT Analyzer:");
  ESP_LOGCONFIG(TAG, "  FFT size: %u samples", this->fft_size_);
  ESP_LOGCONFIG(TAG, "  Sample rate: 16000 Hz (fixed)");
  ESP_LOGCONFIG(TAG, "  Frequency resolution: %.1f Hz/bin",
                16000.0f / (float) this->fft_size_);
  ESP_LOGCONFIG(TAG, "  Named bands: %u (60Hz-16kHz, DEBUG log only)",
                NAMED_BAND_COUNT);
  ESP_LOGCONFIG(TAG, "  Decay: %.2f", this->decay_);
  ESP_LOGCONFIG(TAG, "  Window overlap: %s", this->overlap_ ? "YES (50%)" : "NO");
  ESP_LOGCONFIG(TAG, "  Noise floor: %.4f%s", this->noise_floor_,
                this->noise_floor_ <= 0.0f ? " (gate disabled)" : "");
  if (this->bar_count_ > 0) {
    ESP_LOGCONFIG(TAG, "  Visualizer bars: %u (%.0f Hz - %.0f Hz, log-spaced)",
                  this->bar_count_, this->bar_low_hz_, this->bar_high_hz_);
  } else {
    ESP_LOGCONFIG(TAG, "  Visualizer bars: disabled");
  }
}

}  // namespace fft_analyzer
}  // namespace esphome
