#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cdr {

// Sample rate converter for a rational ratio (CD audio to Opus: 44100 ->
// 48000 Hz, i.e. 160/147), written for the core so that it has no external
// dependency. Polyphase FIR: a Kaiser-windowed sinc evaluated at the exact
// position of every output sample (one phase per output position modulo the
// ratio), so there is no interpolation between table entries.
//
// The default design keeps 0..20 kHz flat (ripple well below 0.01 dB) and
// attenuates everything at or above the lower Nyquist frequency (22.05 kHz)
// by more than 100 dB, so images and aliases end up below the 16-bit noise
// floor. The filter is linear phase and centred on each output sample: the
// output is not delayed, output sample n corresponds to input time
// n * inRate / outRate.
class Resampler {
public:
    struct Design {
        double passbandHz = 20000;    // flat up to here
        double stopbandHz = 22050;    // attenuated from here (<= min(inRate, outRate) / 2)
        double attenuationDb = 105;   // Kaiser window design target
    };

    Resampler(unsigned inRate, unsigned outRate, unsigned channels);
    Resampler(unsigned inRate, unsigned outRate, unsigned channels, const Design& design);

    // Feeds `frames` interleaved input frames and appends the output frames
    // that are complete (interleaved) to `out`.
    void process(const float* in, size_t frames, std::vector<float>& out);

    // Ends the input (the signal is continued with silence) and appends the
    // remaining output, so that the total output is exactly
    // outputLength(total input frames). No further input is accepted.
    void finish(std::vector<float>& out);

    // ceil(inFrames * outRate / inRate): the output covers the whole input.
    // This is also how opusenc sets the end of an Ogg Opus stream, and a
    // decoder converting back to the input rate (floor) gets exactly
    // inFrames samples again.
    uint64_t outputLength(uint64_t inFrames) const;

    unsigned channels() const { return channels_; }
    unsigned tapsPerPhase() const { return taps_; }
    uint64_t inputFrames() const { return inputFrames_; }
    uint64_t outputFrames() const { return outputFrames_; }

private:
    void produce(uint64_t available, std::vector<float>& out);

    unsigned channels_;
    unsigned up_, down_;  // output = input * up_ / down_ (reduced ratio)
    unsigned half_;       // taps on each side of the output position
    unsigned taps_;       // 2 * half_
    std::vector<float> coeffs_;  // up_ phases of taps_ coefficients
    std::vector<float> buffer_;  // interleaved input, the first frame is input index bufferStart_
    int64_t bufferStart_;
    uint64_t inputFrames_ = 0;
    uint64_t outputFrames_ = 0;
    bool finished_ = false;
};

}  // namespace cdr
