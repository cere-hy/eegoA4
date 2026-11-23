// S3cPF7 P0 host regression — the EXACT production RAII guard used by the USB
// MSC read10/write10 callbacks. Verifies:
//  (1) guard takes on construction, gives exactly once on scope exit;
//  (2) every early-return path still gives (RAII, no bypass);
//  (3) a failed take leaves acquired()==false and NEVER gives;
//  (4) take/give happen on the SAME thread/task ("who takes, gives");
//  (5) null lockFn preserves legacy unprotected behaviour (source compat with
//      the old begin(dev) call site).
//
// This compiles MscIoLock.h from the production include dir — no copy/paste of
// the guard logic — so a regression in the header fails this test.

#include <MscIoLock.h>

#include <atomic>
#include <cstdio>
#include <thread>

using freeink::IoLockFn;
using freeink::IoLockGuard;

static int fails = 0, checks = 0;
#define CHECK(cond, msg) do { ++checks; if (!(cond)) { ++fails; std::printf("  RED: %s\n", msg); } } while (0)

struct LockState {
  std::atomic<int> takes{0};
  std::atomic<int> gives{0};
  std::atomic<bool> failNext{false};
  std::atomic<unsigned long> takeThread{0};
  std::atomic<unsigned long> giveThread{0};
  std::atomic<unsigned> depth{0};  // recursive depth
};

static bool lockFn(void* ctx, bool take) {
  auto* s = static_cast<LockState*>(ctx);
  if (take) {
    if (s->failNext.exchange(false)) return false;
    s->takes.fetch_add(1);
    s->takeThread.store(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    s->depth.fetch_add(1);
    return true;
  }
  s->depth.fetch_sub(1);
  s->gives.fetch_add(1);
  s->giveThread.store(std::hash<std::thread::id>{}(std::this_thread::get_id()));
  return true;
}

int main() {
  // (1)+(4) normal scope + same-thread ownership
  {
    LockState s;
    {
      IoLockGuard g(&lockFn, &s);
      CHECK(g.acquired(), "normal take succeeds");
      CHECK(s.takes.load() == 1, "exactly one take");
      CHECK(s.gives.load() == 0, "no give before scope exit");
      CHECK(s.depth.load() == 1, "lock held inside scope");
    }
    CHECK(s.gives.load() == 1, "exactly one give on scope exit");
    CHECK(s.depth.load() == 0, "lock released after scope");
    CHECK(s.takeThread.load() == s.giveThread.load(), "take and give on same thread/task");
  }

  // (2) every early return releases (simulate the multi-return callback body)
  {
    LockState s;
    auto earlyReturnA = [&]() -> int {
      IoLockGuard g(&lockFn, &s);
      if (!g.acquired()) return -1;
      return 42;  // first return path
    };
    auto earlyReturnB = [&]() -> int {
      IoLockGuard g(&lockFn, &s);
      if (!g.acquired()) return -1;
      for (int i = 0; i < 3; ++i) { if (i == 1) return 7; }  // mid-loop return
      return 9;
    };
    CHECK(earlyReturnA() == 42, "path A returns value");
    CHECK(earlyReturnB() == 7, "path B returns value");
    CHECK(s.takes.load() == 2, "two takes across two callbacks");
    CHECK(s.gives.load() == 2, "BOTH early-return paths gave (no guard bypass)");
    CHECK(s.depth.load() == 0, "no leaked lock after early returns");
  }

  // (3) failed take: acquired()==false, no give, no depth change
  {
    LockState s;
    s.failNext.store(true);
    {
      IoLockGuard g(&lockFn, &s);
      CHECK(!g.acquired(), "failed take => acquired()==false (callback must return -1)");
      CHECK(s.takes.load() == 0, "failed take not counted as acquired");
      CHECK(s.depth.load() == 0, "failed take did not change depth");
    }
    CHECK(s.gives.load() == 0, "failed take never gives (no non-owner release)");
  }

  // (5) null lockFn: legacy behaviour, no take/give, still "acquired"
  {
    IoLockGuard g(nullptr, nullptr);
    CHECK(g.acquired(), "null lockFn keeps legacy unprotected path usable");
  }

  // recursive-style reentrancy across nested scopes stays balanced
  {
    LockState s;
    {
      IoLockGuard g1(&lockFn, &s);
      IoLockGuard g2(&lockFn, &s);
      CHECK(s.depth.load() == 2, "nested guards raise recursive depth to 2");
    }
    CHECK(s.depth.load() == 0 && s.gives.load() == 2, "nested guards balanced");
  }

  std::printf("msc_io_lock host test: %d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}
