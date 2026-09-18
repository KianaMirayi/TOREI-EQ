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
// >>> Currently at the DEFAULT (0 = diagnostics OFF). Set it to 1 and rebuild to get a
// >>> diagnostic build for a logging session (LANES / MSPROBE / SOLO / CURVE / ENGINE).
//
// Kept ON regardless of the switch:
//   * logEq() itself, and the 8 MB rotation guard inside it;
//   * the short `RECV Parameter_Change` / `RECV Command` transport lines
//     (the direct evidence that the UI -> C++ bridge works);
//   * the one-shot `PROC` / `PROC-SILENT` diagnostics (gated by procLogged);
//   * the "SLOWTICK" alarm (PluginEditor.cpp `kSlowTickAlarmMs`): one line, written only
//     when a single UI tick exceeds 25 ms. This freeze turned out to be a message-thread
//     OCCUPANCY problem (WEBVIEW2_MULTI_INSTANCE_FREEZE_HANDOFF.md 13.1), so keeping one
//     always-on occupancy guard is worth it -- in a healthy build it never fires.
// ---------------------------------------------------------------------------
#ifndef TOREI_EQ_DEBUG_LOG
    #define TOREI_EQ_DEBUG_LOG 0
#endif

// ---------------------------------------------------------------------------
// TOREI_DIAG_PUSH_MODE  (default 0) -- 推送量实验/节流
//   0 -- 原样：每个 tick 推送全部事件（Audio_Level / 2 条频谱 / 曲线 / listen），
//        其中曲线已改为"只在变化时推"（见下面 DSH 的注意事项）。
//   1 -- 停掉"重"事件（两条频谱 + 曲线），只留电平与监听两条极小的事件。
//        用于二分定位：冻结消失 => 与每 tick 的事件流有关；仍冻结 => 与推送无关。
//        注意它也跳过了曲线计算本身，所以"不卡"只能说明"重活/大流量这一段有关"，
//        不能区分 IPC 流量与消息线程 CPU（要区分需要模式 3：只停 emit、照常计算）。
//   2 -- 轻量推送（候选修复）：频谱每 2 个 tick（≈12 Hz），并去掉与 _Mid 完全重复的
//        旧事件 EQ_Curve_Data（5 个变 4 个）。
//        曲线仍保持 0.01 dB 精度 —— 精度是曲线台阶感的根因，不能为省字节退回 0.1。
//
// 注意（DSH，2026-09-18）：曲线的"每 4 tick"节流已取消，改成**只在变化时推**
// （见 PluginEditor.cpp 的曲线去重段 + EqEngine::getCurveRevision()）。去重比抽样好：
// 空闲时曲线推送直接降为零，而拖节点时仍然每个 tick 都推（跟手）。因此模式 0 也不再
// 每 tick 重推曲线，模式 2 与模式 0 的差别只剩"频谱 12 Hz vs 21 Hz"这一条。
//
// 这个宏只是默认值：运行时可以用 %APPDATA%\TOREI-EQ\push_mode.txt（内容 0/1/2）
// 覆盖它，编辑器每秒重读一次，改动后不用重启 DAW 也不用重编译 —— **但这条运行时
// 覆盖路径只在诊断构建（TOREI_EQ_DEBUG_LOG=1）里编译**（DSH，2026-09-18：产品构建
// 不该每秒去碰一次文件系统）。产品构建永远使用这里的编译期默认值；心跳日志里的
// `push=` 字段同样只在诊断构建里存在。
// ---------------------------------------------------------------------------
#ifndef TOREI_DIAG_PUSH_MODE
    #define TOREI_DIAG_PUSH_MODE 0
#endif
