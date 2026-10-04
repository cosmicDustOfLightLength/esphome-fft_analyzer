#pragma once

#include "esphome/core/component.h"
#include "esphome/components/microphone/microphone.h"

#include <vector>

namespace esphome {
namespace fft_analyzer {

class FFTAnalyzer : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;

  void set_microphone(microphone::Microphone *mic) {
    this->mic_ = mic;
  }

  void set_fft_size(size_t size) {
    this->fft_size_ = size;
  }

  // Wspolczynnik opadania (0.0 - 1.0). Im blizej 1.0, tym wolniej
  // slupek opada po piku. Domyslnie 0.85.
  void set_decay(float decay) {
    this->decay_ = decay;
  }

  // Liczba generycznych "slupkow" do wizualizacji (nie sa to sensory
  // HA - dostepne wylacznie z C++/lambdy, zeby nie mnozyc encji).
  void set_bar_count(size_t count) {
    this->bar_count_ = count;
    this->bars_.resize(count, 0.0f);
    this->bar_edges_low_.resize(count, 0);
    this->bar_edges_high_.resize(count, 0);
    this->bar_edges_ready_ = false;
  }

  void set_bar_freq_range(float low_hz, float high_hz) {
    this->bar_low_hz_ = low_hz;
    this->bar_high_hz_ = high_hz;
  }

  // Dostep z lambdy displaya: id(fft).get_bar_count() / get_bar(i)
  size_t get_bar_count() const {
    return this->bar_count_;
  }

  float get_bar(size_t index) const {
    if (index >= this->bars_.size()) {
      return 0.0f;
    }
    return this->bars_[index];
  }

 protected:
  void process_audio_(const std::vector<uint8_t> &data);
  void calculate_fft_();
  void compute_bar_edges_();

  microphone::Microphone *mic_{nullptr};

  size_t fft_size_{512};

  std::vector<float> samples_;
  std::vector<float> window_;
  std::vector<float> real_;
  std::vector<float> imag_;

  bool new_data_{false};

  // Surowe wyniki 9 pasm FFT (magnitude z biezacej ramki).
  float bands_[9]{};

  // Wygladzone wartosci (peak-hold + decay), widoczne w logu DEBUG.
  float smoothed_[9]{};

  // Wspolczynnik opadania per klatka FFT.
  float decay_{0.85f};

  // Generyczne slupki (log-spaced) do wizualizacji.
  size_t bar_count_{0};
  float bar_low_hz_{50.0f};
  float bar_high_hz_{18000.0f};
  bool bar_edges_ready_{false};

  std::vector<float> bars_;
  std::vector<int> bar_edges_low_;
  std::vector<int> bar_edges_high_;

  uint32_t sample_count_{0};
};

}  // namespace fft_analyzer
}  // namespace esphome
