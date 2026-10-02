#include "cdreader/clock.h"

#include <chrono>

namespace cdr {

namespace {

class SteadyClock : public Clock {
public:
    uint64_t nowMicros() override {
        using namespace std::chrono;
        return uint64_t(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
    }
};

}  // namespace

Clock& steadyClock() {
    static SteadyClock clock;
    return clock;
}

}  // namespace cdr
