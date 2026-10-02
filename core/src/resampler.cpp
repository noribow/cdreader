#include "cdreader/resampler.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace cdr {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Modified Bessel function of the first kind, order 0 (power series).
double besselI0(double x) {
    double sum = 1, term = 1;
    const double q = x * x / 4;
    for (int k = 1; k < 200; ++k) {
        term *= q / (double(k) * k);
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

double sinc(double x) { return x == 0 ? 1.0 : std::sin(kPi * x) / (kPi * x); }

}  // namespace

Resampler::Resampler(unsigned inRate, unsigned outRate, unsigned channels)
    : Resampler(inRate, outRate, channels, Design{}) {}

Resampler::Resampler(unsigned inRate, unsigned outRate, unsigned channels, const Design& design)
    : channels_(channels) {
    if (inRate == 0 || outRate == 0 || channels == 0) throw std::invalid_argument("invalid resampler parameters");
    if (!(design.passbandHz > 0 && design.stopbandHz > design.passbandHz &&
          design.stopbandHz <= std::min(inRate, outRate) / 2.0 + 1e-9 && design.attenuationDb > 21))
        throw std::invalid_argument("invalid resampler filter design");
    const unsigned g = std::gcd(inRate, outRate);
    up_ = outRate / g;
    down_ = inRate / g;
    if (up_ > 4096) throw std::invalid_argument("resampling ratio too complex");

    // Kaiser window design (time in input samples).
    const double a = design.attenuationDb;
    const double beta = a > 50 ? 0.1102 * (a - 8.7) : 0.5842 * std::pow(a - 21, 0.4) + 0.07886 * (a - 21);
    const double transition = 2 * kPi * (design.stopbandHz - design.passbandHz) / inRate;
    const double length = (a - 7.95) / (2.285 * transition) + 1;
    half_ = unsigned(std::ceil(length / 2));
    taps_ = 2 * half_;
    const double cutoff = (design.passbandHz + design.stopbandHz) / inRate;  // 2 * fc / inRate
    const double i0Beta = besselI0(beta);

    coeffs_.resize(size_t(up_) * taps_);
    std::vector<double> h(taps_);
    for (unsigned p = 0; p < up_; ++p) {
        double sum = 0;
        for (unsigned j = 0; j < taps_; ++j) {
            // Tap j weights input sample i - (half - 1) + j for an output at i + p / up.
            const double t = double(j) - double(half_ - 1) - double(p) / up_;
            const double u = t / half_;
            const double window = std::abs(u) >= 1 ? 0.0 : besselI0(beta * std::sqrt(1 - u * u)) / i0Beta;
            h[j] = cutoff * sinc(cutoff * t) * window;
            sum += h[j];
        }
        // Exact unity gain at DC for every phase.
        for (unsigned j = 0; j < taps_; ++j) coeffs_[size_t(p) * taps_ + j] = float(h[j] / sum);
    }

    // The first outputs look back before the start of the input: silence.
    bufferStart_ = -int64_t(half_ - 1);
    buffer_.assign(size_t(half_ - 1) * channels_, 0.0f);
}

uint64_t Resampler::outputLength(uint64_t inFrames) const {
    return (inFrames * up_ + down_ - 1) / down_;
}

void Resampler::process(const float* in, size_t frames, std::vector<float>& out) {
    if (finished_) throw std::logic_error("resampler input already finished");
    buffer_.insert(buffer_.end(), in, in + frames * channels_);
    inputFrames_ += frames;
    produce(inputFrames_, out);
}

void Resampler::finish(std::vector<float>& out) {
    if (finished_) return;
    finished_ = true;
    buffer_.resize(buffer_.size() + size_t(half_ + 1) * channels_, 0.0f);
    produce(inputFrames_ + half_ + 1, out);
}

// Produces the outputs whose taps are all within the first `available`
// input frames (silence included after finish()).
void Resampler::produce(uint64_t available, std::vector<float>& out) {
    const uint64_t limit = finished_ ? outputLength(inputFrames_) : std::numeric_limits<uint64_t>::max();
    for (; outputFrames_ < limit; ++outputFrames_) {
        const uint64_t position = outputFrames_ * down_;
        const uint64_t i = position / up_;
        if (i + half_ >= available) break;
        const float* c = coeffs_.data() + size_t(position % up_) * taps_;
        const float* x = buffer_.data() + size_t(int64_t(i) - int64_t(half_ - 1) - bufferStart_) * channels_;
        for (unsigned ch = 0; ch < channels_; ++ch) {
            float acc = 0;
            for (unsigned j = 0; j < taps_; ++j) acc += c[j] * x[size_t(j) * channels_ + ch];
            out.push_back(acc);
        }
    }
    // Drop input that no future output needs (in batches, erasing is a move).
    const int64_t next = int64_t(outputFrames_ * down_ / up_) - int64_t(half_ - 1);
    if (next - bufferStart_ >= 8192) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + ptrdiff_t((next - bufferStart_) * channels_));
        bufferStart_ = next;
    }
}

}  // namespace cdr
