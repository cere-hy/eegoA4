#pragma once

// S3cPF7 — RAII I/O lock for USB MSC callbacks.
//
// The TinyUSB "usbd" task (created with xTaskCreate, no core affinity) services
// READ(10)/WRITE(10) on a different task — and possibly a different core —
// than the application's normal SD readers (pinned to core 1). SdFat is not
// thread-safe, so every MSC callback must acquire the SAME storage mutex the
// application uses, and must take/give it within ONE task so recursive-mutex
// ownership (xTaskGetCurrentTaskHandle / pxCurrentTCBs[xPortGetCoreID]) holds.
//
// This header is deliberately Arduino-free so the exact production guard can
// be regression-tested on the host.

namespace freeink {

// take=true acquires (false => caller must abort the transfer);
// take=false releases. Ownership stays in the task that took the guard.
using IoLockFn = bool (*)(void* context, bool take);

class IoLockGuard {
 public:
  IoLockGuard(IoLockFn fn, void* ctx) : fn_(fn), ctx_(ctx) {
    // A null lockFn preserves the historical unprotected behaviour (callers
    // that never injected a lock). With a lock installed, a failed take must
    // abort the callback — acquired() reports false.
    acquired_ = fn_ ? fn_(ctx_, true) : true;
  }
  ~IoLockGuard() {
    // Only release what this guard actually acquired, in the same task.
    if (acquired_ && fn_) fn_(ctx_, false);
  }
  bool acquired() const { return acquired_; }

  IoLockGuard(const IoLockGuard&) = delete;
  IoLockGuard& operator=(const IoLockGuard&) = delete;

 private:
  IoLockFn fn_;
  void* ctx_;
  bool acquired_ = false;
};

}  // namespace freeink
