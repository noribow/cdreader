#pragma once

#include <cstdint>

namespace cdr {

// Monotonic time source in microseconds. The drive cache detection (#34)
// times reads with it; tests inject a clock that the fake drive advances by
// a simulated time per command, so that detection is deterministic.
class Clock {
public:
    virtual ~Clock() = default;
    virtual uint64_t nowMicros() = 0;
};

// std::chrono::steady_clock.
Clock& steadyClock();

}  // namespace cdr
