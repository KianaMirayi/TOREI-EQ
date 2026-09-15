#pragma once

// ---------------------------------------------------------------------------
// TOREI-EQ build-time configuration.
//
// TOREI_EQ_DEBUG_LOG
//   0 (default) -- diagnostic-only logging is compiled OUT, together with the code
//                  that exists purely to feed it (the M/S RMS probe passes, the
//                  lane-mix atomics, the curve-peak bookkeeping). That means zero
//                  runtime cost and zero writes to eq_debug.txt in normal use.
//   1           -- recompile to get the full diagnostics back. This was used for
//                  the M/S verification round (MID_SIDE_HANDOFF.md 13 / 18).
//
// >>> CURRENTLY SET TO 1 (diagnostics ON) at the user's request, for a logging
// >>> session. Set it back to 0 and rebuild to return to the quiet default.
//
// Kept ON regardless of the switch:
//   * logEq() itself, and the 8 MB rotation guard inside it;
//   * the short `RECV Parameter_Change` / `RECV Command` transport lines
//     (the direct evidence that the UI -> C++ bridge works);
//   * the one-shot `PROC` / `PROC-SILENT` diagnostics (gated by procLogged).
// ---------------------------------------------------------------------------
#ifndef TOREI_EQ_DEBUG_LOG
    #define TOREI_EQ_DEBUG_LOG 0
#endif
