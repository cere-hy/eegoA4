#include "WifiTransferMode.h"

#include <atomic>

namespace wifitransfer {

namespace {
std::atomic<bool> s_active{false};
std::atomic<bool> s_abnormalExit{false};
}

void activate() { s_active.store(true, std::memory_order_release); }

bool active() { return s_active.load(std::memory_order_acquire); }

void markAbnormalExit() { s_abnormalExit.store(true, std::memory_order_release); }

bool consumeAbnormalExit() { return s_abnormalExit.exchange(false, std::memory_order_acq_rel); }

}  // namespace wifitransfer
