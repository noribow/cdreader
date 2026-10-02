#pragma once

// Synthetic CD-DA test signals (interleaved 16-bit little-endian stereo PCM).

#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace testsig {

struct Signal {
    std::string name;
    std::vector<uint8_t> pcm;
};

inline int16_t clamp16(double v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return int16_t(std::lround(v));
}

// Deterministic white noise in [-1, 1).
class Noise {
public:
    explicit Noise(uint32_t seed) : state_(seed) {}
    double next() {
        state_ = state_ * 1664525u + 1013904223u;
        return double(int32_t(state_)) / 2147483648.0;
    }

private:
    uint32_t state_;
};

inline Signal make(const std::string& name, size_t samples, const std::function<void(size_t, int16_t&, int16_t&)>& gen) {
    Signal s{name, std::vector<uint8_t>(samples * 4)};
    for (size_t i = 0; i < samples; ++i) {
        int16_t l = 0, r = 0;
        gen(i, l, r);
        uint8_t* p = s.pcm.data() + i * 4;
        p[0] = uint8_t(uint16_t(l));
        p[1] = uint8_t(uint16_t(l) >> 8);
        p[2] = uint8_t(uint16_t(r));
        p[3] = uint8_t(uint16_t(r) >> 8);
    }
    return s;
}

constexpr double kTwoPi = 6.283185307179586;

// A few seconds of "music": harmonic tones with envelopes plus a little noise.
inline Signal music(size_t samples) {
    Noise n(7);
    return make("music", samples, [n](size_t i, int16_t& l, int16_t& r) mutable {
        const double t = double(i) / 44100;
        const double notes[] = {220.0, 277.18, 329.63, 440.0};
        double a = 0, b = 0;
        for (int k = 0; k < 4; ++k) {
            const double env = 0.5 + 0.5 * std::sin(kTwoPi * (0.3 + 0.17 * k) * t);
            for (int h = 1; h <= 4; ++h) {
                a += env / h * std::sin(kTwoPi * notes[k] * h * t);
                b += env / h * std::sin(kTwoPi * notes[k] * h * t + 0.3 * k);
            }
        }
        const double noise = n.next() * 40;
        l = clamp16(a * 2800 + noise);
        r = clamp16(b * 2600 + noise * 0.5 + n.next() * 20);
    });
}

inline std::vector<Signal> all() {
    std::vector<Signal> v;
    v.push_back(make("empty", 0, [](size_t, int16_t&, int16_t&) {}));
    v.push_back(make("silence", 10000, [](size_t, int16_t& l, int16_t& r) { l = r = 0; }));
    v.push_back(make("dc_extremes", 5000, [](size_t, int16_t& l, int16_t& r) {
        l = 32767;
        r = -32768;
    }));
    v.push_back(make("square_full_scale", 3 * 4096 + 123, [](size_t i, int16_t& l, int16_t& r) {
        l = (i / 50) % 2 ? 32767 : -32768;
        r = (i / 37) % 2 ? -32768 : 32767;
    }));
    v.push_back(make("alternating_full_scale", 9000, [](size_t i, int16_t& l, int16_t& r) {
        l = i % 2 ? 32767 : -32768;
        r = i % 2 ? -32768 : 32767;
    }));
    v.push_back(make("sine", 44100 * 2 + 7, [](size_t i, int16_t& l, int16_t& r) {
        l = clamp16(26000 * std::sin(kTwoPi * 1000 * double(i) / 44100));
        r = clamp16(26000 * std::sin(kTwoPi * 1000 * double(i) / 44100 + 1.0));
    }));
    {
        Noise n(1);
        v.push_back(make("white_noise", 50000, [n](size_t, int16_t& l, int16_t& r) mutable {
            l = clamp16(n.next() * 32768);
            r = clamp16(n.next() * 32768);
        }));
    }
    {
        Noise n(2);
        v.push_back(make("low_level_noise", 20000, [n](size_t, int16_t& l, int16_t& r) mutable {
            l = clamp16(n.next() * 3);
            r = clamp16(n.next() * 2);
        }));
    }
    {
        Noise n(3);
        v.push_back(make("identical_channels", 30001, [n](size_t i, int16_t& l, int16_t& r) mutable {
            l = r = clamp16(9000 * std::sin(kTwoPi * 440 * double(i) / 44100) + n.next() * 300);
        }));
    }
    v.push_back(make("one_channel_silent", 20000, [](size_t i, int16_t& l, int16_t& r) {
        l = clamp16(20000 * std::sin(kTwoPi * 300 * double(i) / 44100));
        r = 0;
    }));
    {
        Noise n(4);
        v.push_back(make("wasted_bits", 12000, [n](size_t, int16_t& l, int16_t& r) mutable {
            l = int16_t(int(n.next() * 64) * 256);
            r = int16_t(int(n.next() * 16) * 2048);
        }));
    }
    {
        Noise n(5);
        v.push_back(make("burst_then_silence", 4096 * 2 + 1, [n](size_t i, int16_t& l, int16_t& r) mutable {
            l = i < 700 ? clamp16(n.next() * 20000) : 0;
            r = i % 4096 < 100 ? clamp16(n.next() * 5000) : 0;
        }));
    }
    {
        Noise n(6);
        v.push_back(make("spikes", 10000, [n](size_t i, int16_t& l, int16_t& r) mutable {
            l = i % 997 == 0 ? 32767 : clamp16(n.next() * 4);
            r = i % 1499 == 0 ? -32768 : clamp16(n.next() * 4);
        }));
    }
    for (size_t len : {size_t(1), size_t(2), size_t(5), size_t(15), size_t(16), size_t(17), size_t(100),
                       size_t(588), size_t(4095), size_t(4096), size_t(4097), size_t(8191), size_t(12289)}) {
        Noise n{uint32_t(len)};
        v.push_back(make("length_" + std::to_string(len), len, [n](size_t i, int16_t& l, int16_t& r) mutable {
            l = clamp16(12000 * std::sin(kTwoPi * 523 * double(i) / 44100) + n.next() * 1000);
            r = clamp16(n.next() * 30000);
        }));
    }
    v.push_back(music(44100 * 3));
    return v;
}

}  // namespace testsig
