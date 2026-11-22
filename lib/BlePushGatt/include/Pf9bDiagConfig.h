#pragma once

// b65-s3c-pf9b — phase-1 measurement build switch.
//
// This header is the ONE place that turns the [MEMX] instrumentation on.
// The phase-1 diagnostic build includes it as-is: PF9B_MEMX_DIAG defaults to 1.
// The phase-2 functional build is compiled with -DPF9B_RELEASE=1 (a single
// build flag, no source edit), which switches every guarded block off and emits
// zero [MEMX] lines. Nothing in this header touches the render/advance paths.
//
// Scope: WiFi/BLE lifecycle only (WifiSelectionActivity + BlePushGatt).
#if !defined(PF9B_RELEASE)
#define PF9B_MEMX_DIAG 1
#else
#undef PF9B_MEMX_DIAG
#endif
