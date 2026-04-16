#include "rtreticulum/reticulum.h"

#include "rtreticulum/hal.h"
#include "rtreticulum/transport.h"

namespace RNS {

std::atomic<bool> Reticulum::_running{false};
std::atomic<bool> Reticulum::_stop_requested{false};
uint32_t          Reticulum::_tick_ms = 100;
bool              Reticulum::_use_implicit_proof = true;
bool              Reticulum::_transport_enabled  = false;

void Reticulum::run_once() {
    /* Pump every interface's loop hook. Phase 7 leaves the rest of
     * housekeeping (path expiry, packet cache cleanup) for later phases. */
    for (auto& iface : Transport::interfaces()) {
        if (iface) iface->loop();
    }
}

void Reticulum::task_entry(void* /*arg*/) {
    while (!_stop_requested.load()) {
        run_once();
        rt_hal_delay_ms(_tick_ms);
    }
    _running.store(false);
}

bool Reticulum::start(uint32_t tick_ms, size_t stack_words, int priority) {
    if (_running.exchange(true)) return false;  /* already running */
    _stop_requested.store(false);
    _tick_ms = tick_ms;
    auto* task = rt_hal_task_spawn("rtreticulum", task_entry, nullptr, stack_words, priority);
    if (!task) {
        _running.store(false);
        return false;
    }
    return true;
}

void Reticulum::stop() {
    _stop_requested.store(true);
}

}
