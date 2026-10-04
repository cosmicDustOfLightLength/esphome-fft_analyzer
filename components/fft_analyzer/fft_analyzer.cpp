#include "fft_analyzer.h"

#include "esphome/core/log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace esphome {
namespace fft_analyzer {


static const char *const TAG = "fft_analyzer";

static constexpr float PI = 3.14159265358979323846f;
static constexpr float SAMPLE_RATE = 48000.0f;
static constexpr size_t BAND_COUNT = 9;


void FFTAnalyzer::setup() {
  ESP_LOGCONFIG(TAG, "Setting up FFT analyzer");

  if (this->mic_ == nullptr) {
    ESP_LOGE(TAG, "No microphone configured");
    this->mark_failed();
    return;
  }

  if (this->fft_size_ < 2 ||
      (this->fft_size_ & (this->fft_size_ - 1)) != 0) {
    ESP_LOGE(
        TAG,
        "FFT size must be a power of two, got %u",
        this->fft_size_
    );
    this->mark_failed();
    return;
  }

  // --------------------------------------------------
  // Allocate memory once
  // --------------------------------------------------

  this->samples_.reserve(this->fft_size_);
  this->window_.resize(this->fft_size_);
  this->real_.resize(this->fft_size_);
  this->imag_.resize(this->fft_size_);

  // --------------------------------------------------
  // Hann window
  // --------------------------------------------------

  const float denominator =
      static_cast<float>(this->fft_size_ - 1);

  for (size_t i = 0; i < this->fft_size_; i++) {
    this->window_[i] =
        0.5f *
        (
            1.0f -
            cosf(
                (2.0f * PI * static_cast<float>(i)) /
                denominator
            )
        );
  }

  // --------------------------------------------------
  // Receive microphone samples
  // --------------------------------------------------

  /*
   * Microphone jest uruchamiany przez ESPHome.
   *
   * Nie wywolujemy mic_->start().
   * add_data_callback() dostarcza nam kolejne
   * porcje danych PCM.
   */

  this->mic_->add_data_callback(
      [this](const std::vector<uint8_t> &data) {
        this->process_audio_(data);
      }
  );

  ESP_LOGI(
      TAG,
      "FFT initialized: %u samples",
      this->fft_size_
  );

  ESP_LOGI(
      TAG,
      "Frequency resolution: %.2f Hz/bin",
      SAMPLE_RATE / static_cast<float>(this->fft_size_)
  );

  if (this->bar_count_ > 0) {
    this->compute_bar_edges_();
  }
}

void FFTAnalyzer::compute_bar_edges_() {
  const float bin_hz =
      SAMPLE_RATE / static_cast<float>(this->fft_size_);

  const int max_bin =
      static_cast<int>(this->fft_size_ / 2 - 1);

  // Rozklad logarytmiczny granic pasm miedzy bar_low_hz_ a bar_high_hz_.
  const float log_low = log10f(this->bar_low_hz_);
  const float log_high = log10f(this->bar_high_hz_);

  int prev_bin = std::max(1, static_cast<int>(ceilf(this->bar_low_hz_ / bin_hz)));

  for (size_t i = 0; i < this->bar_count_; i++) {
    const float frac =
        static_cast<float>(i + 1) / static_cast<float>(this->bar_count_);

    const float edge_hz =
        powf(10.0f, log_low + frac * (log_high - log_low));

    int edge_bin =
        static_cast<int>(roundf(edge_hz / bin_hz));

    edge_bin = std::min(max_bin, std::max(prev_bin, edge_bin));

    this->bar_edges_low_[i] = prev_bin;
    this->bar_edges_high_[i] = edge_bin;

    prev_bin = edge_bin + 1;
    if (prev_bin > max_bin) {
      prev_bin = max_bin;
    }
  }

  this->bar_edges_ready_ = true;

  ESP_LOGCONFIG(TAG, "Computed %u visualizer bars (%.0f Hz - %.0f Hz)",
                this->bar_count_, this->bar_low_hz_, this->bar_high_hz_);
}

void FFTAnalyzer::process_audio_(
    const std::vector<uint8_t> &data
) {
  if (data.empty()) {
    return;
  }

  /*
   * Dane z ES7210:
   *
   * 16-bit PCM
   * little endian
   */

  const size_t sample_count =
      data.size() / sizeof(int16_t);

  for (size_t i = 0; i < sample_count; i++) {

    /*
     * Jezeli poprzednia ramka czeka na
     * obliczenie FFT, nie dokladamy kolejnych
     * probek.
     */
    if (this->new_data_) {
      return;
    }

    const size_t offset =
        i * sizeof(int16_t);

    const uint16_t raw =
        static_cast<uint16_t>(data[offset]) |
        (
            static_cast<uint16_t>(
                data[offset + 1]
            ) << 8
        );

    const int16_t pcm =
        static_cast<int16_t>(raw);

    const float sample =
        static_cast<float>(pcm) / 32768.0f;

    this->samples_.push_back(sample);

    if (this->samples_.size() >= this->fft_size_) {
      this->new_data_ = true;
      return;
    }
  }
}

void FFTAnalyzer::loop() {
  if (!this->new_data_) {
    return;
  }

  /*
   * Zabezpieczenie przed ponownym przetwarzaniem.
   */
  this->new_data_ = false;

  if (this->samples_.size() < this->fft_size_) {
    return;
  }

  this->calculate_fft_();
  this->samples_.clear();
  //this->calculate_fft_();
  // Zachowaj polowe probek (50% overlap) zamiast czyscic caly bufor -
  // kolejna klatka FFT policzy sie szybciej i animacja bedzie plynniejsza.
  // half = this->fft_size_ / 2;
  //std::vector<float> tail(this->samples_.end() - half, this->samples_.end());
  //this->samples_ = std::move(tail);
}

void FFTAnalyzer::calculate_fft_() {
  const size_t N = this->fft_size_;

  if (N < 2) {
    return;
  }

  // --------------------------------------------------
  // Remove DC offset
  // --------------------------------------------------

  float mean = 0.0f;

  for (size_t i = 0; i < N; i++) {
    mean += this->samples_[i];
  }

  mean /= static_cast<float>(N);

  // --------------------------------------------------
  // Apply Hann window
  // --------------------------------------------------

  for (size_t i = 0; i < N; i++) {
    this->real_[i] =
        (this->samples_[i] - mean) *
        this->window_[i];

    this->imag_[i] = 0.0f;
  }

  // --------------------------------------------------
  // Bit reversal
  // --------------------------------------------------

  size_t j = 0;

  for (size_t i = 1; i < N; i++) {

    size_t bit = N >> 1;

    while (j & bit) {
      j ^= bit;
      bit >>= 1;
    }

    j ^= bit;

    if (i < j) {

      std::swap(
          this->real_[i],
          this->real_[j]
      );

      std::swap(
          this->imag_[i],
          this->imag_[j]
      );
    }
  }

  // --------------------------------------------------
  // Radix-2 Cooley-Tukey FFT
  // --------------------------------------------------

  for (size_t len = 2; len <= N; len <<= 1) {

    const float angle =
        -2.0f *
        PI /
        static_cast<float>(len);

    const float wlen_re =
        cosf(angle);

    const float wlen_im =
        sinf(angle);

    for (size_t i = 0; i < N; i += len) {

      float w_re = 1.0f;
      float w_im = 0.0f;

      const size_t half = len >> 1;

      for (size_t k = 0; k < half; k++) {

        const size_t u =
            i + k;

        const size_t v =
            u + half;

        const float v_re =
            this->real_[v] * w_re -
            this->imag_[v] * w_im;

        const float v_im =
            this->real_[v] * w_im +
            this->imag_[v] * w_re;

        const float u_re =
            this->real_[u];

        const float u_im =
            this->imag_[u];

        this->real_[u] =
            u_re + v_re;

        this->imag_[u] =
            u_im + v_im;

        this->real_[v] =
            u_re - v_re;

        this->imag_[v] =
            u_im - v_im;

        // w *= wlen

        const float next_w_re =
            w_re * wlen_re -
            w_im * wlen_im;

        const float next_w_im =
            w_re * wlen_im +
            w_im * wlen_re;

        w_re = next_w_re;
        w_im = next_w_im;
      }
    }
  }

  // --------------------------------------------------
  // Frequency bands
  // --------------------------------------------------

  /*
   * 512 probek @ 48 kHz:
   *
   * 48000 / 512 = 93.75 Hz/bin
   *
   * Pasma:
   *
   * 0 = 60 Hz
   * 1 = 120 Hz
   * 2 = 250 Hz
   * 3 = 500 Hz
   * 4 = 1 kHz
   * 5 = 2 kHz
   * 6 = 4 kHz
   * 7 = 8 kHz
   * 8 = 16 kHz
   */

  const float bin_hz =
      SAMPLE_RATE /
      static_cast<float>(N);

  const float low[BAND_COUNT] = {
      50.0f,
      90.0f,
      180.0f,
      350.0f,
      700.0f,
      1400.0f,
      2800.0f,
      5600.0f,
      11000.0f
  };

  const float high[BAND_COUNT] = {
      90.0f,
      180.0f,
      350.0f,
      700.0f,
      1400.0f,
      2800.0f,
      5600.0f,
      11000.0f,
      20000.0f
  };

  for (size_t band = 0;
       band < BAND_COUNT;
       band++) {

    int first_bin =
        static_cast<int>(
            std::ceil(
                low[band] / bin_hz
            )
        );

    int last_bin =
        static_cast<int>(
            std::floor(
                high[band] / bin_hz
            )
        );

    first_bin =
        std::max(
            1,
            first_bin
        );

    last_bin =
        std::min(
            static_cast<int>(N / 2 - 1),
            last_bin
        );

    float value = 0.0f;

    if (first_bin <= last_bin) {

      float sum = 0.0f;
      int count = 0;

      for (int k = first_bin;
           k <= last_bin;
           k++) {

        const float re =
            this->real_[k];

        const float im =
            this->imag_[k];

        const float magnitude =
            sqrtf(
                re * re +
                im * im
            );

        sum += magnitude;
        count++;
      }

      if (count > 0) {
        value =
            sum /
            static_cast<float>(count);
      }
    }

    this->bands_[band] = value;

    // ------------------------------------------------
    // Peak-hold + decay wygladzanie
    // ------------------------------------------------
    //
    // Jesli nowa wartosc jest wyzsza od aktualnie
    // wygladzonej - natychmiastowy "atak" (skok w gore).
    // W przeciwnym razie plynne opadanie wg wspolczynnika
    // decay_ (np. 0.85 = powolne opadanie, 0.5 = szybkie).

    if (value > this->smoothed_[band]) {
      this->smoothed_[band] = value;
    } else {
      this->smoothed_[band] =
          this->smoothed_[band] * this->decay_ +
          value * (1.0f - this->decay_);
    }
  }

  // --------------------------------------------------
  // Generyczne slupki do wizualizacji (log-spaced)
  // --------------------------------------------------

  if (this->bar_count_ > 0 && this->bar_edges_ready_) {
    for (size_t bar = 0; bar < this->bar_count_; bar++) {
      const int first_bin = this->bar_edges_low_[bar];
      const int last_bin = this->bar_edges_high_[bar];

      float sum = 0.0f;
      int count = 0;

      for (int k = first_bin; k <= last_bin; k++) {
        const float re = this->real_[k];
        const float im = this->imag_[k];
        sum += sqrtf(re * re + im * im);
        count++;
      }

      const float value = (count > 0) ? (sum / static_cast<float>(count)) : 0.0f;

      if (value > this->bars_[bar]) {
        this->bars_[bar] = value;
      } else {
        this->bars_[bar] =
            this->bars_[bar] * this->decay_ +
            value * (1.0f - this->decay_);
      }
    }
  }

  // --------------------------------------------------
  // Debug
  // --------------------------------------------------

  ESP_LOGD(
      TAG,
      "FFT "
      "60=%0.3f "
      "120=%0.3f "
      "250=%0.3f "
      "500=%0.3f "
      "1k=%0.3f "
      "2k=%0.3f "
      "4k=%0.3f "
      "8k=%0.3f "
      "16k=%0.3f",
      this->smoothed_[0],
      this->smoothed_[1],
      this->smoothed_[2],
      this->smoothed_[3],
      this->smoothed_[4],
      this->smoothed_[5],
      this->smoothed_[6],
      this->smoothed_[7],
      this->smoothed_[8]
  );
}

void FFTAnalyzer::dump_config() {
  ESP_LOGCONFIG(TAG, "FFT Analyzer:");

  ESP_LOGCONFIG(
      TAG,
      "  FFT size: %u",
      this->fft_size_
  );

  ESP_LOGCONFIG(
      TAG,
      "  Sample rate: %.0f Hz",
      SAMPLE_RATE
  );

  ESP_LOGCONFIG(
      TAG,
      "  Frequency resolution: %.2f Hz/bin",
      SAMPLE_RATE /
          static_cast<float>(this->fft_size_)
  );

  ESP_LOGCONFIG(
      TAG,
      "  Bands: 9"
  );

  ESP_LOGCONFIG(
      TAG,
      "  Decay: %.2f",
      this->decay_
  );

  if (this->bar_count_ > 0) {
    ESP_LOGCONFIG(
        TAG,
        "  Visualizer bars: %u (%.0f Hz - %.0f Hz)",
        this->bar_count_,
        this->bar_low_hz_,
        this->bar_high_hz_
    );
  }
}

}  // namespace fft_analyzer
}  // namespace esphome
