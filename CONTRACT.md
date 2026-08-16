# TOREI-EQ JSI 通信接口契约

> v1.0 — 此契约是 C++ 和 UI 双方开发的唯一接口标准。
> 任何一方修改数据结构必须同步更新本文档并通知对方。

---

## 通道总览

| 通道名 | 方向 | 数据类型 | 频率 | 用途 |
|--------|------|---------|------|------|
| `EQ_Curve_Data` | C++ → UI | `Float32Array` | 按需（视图变化时） | EQ 频率响应曲线极值点 |
| `Phase_Curve_Data` | C++ → UI | `Float32Array` | 按需（视图变化时） | Phase Display 相位响应 |
| `Spectrum_Data` | C++ → UI | `Float32Array` | ~40Hz | 实时频谱分析数据 |
| `Peak_Level` | C++ → UI | `Float32` | ~86Hz（每帧） | 输出峰值电平 |
| `Parameter_Change` | UI → C++ | `Float32Array` | 拖拽时 (≥1ms 节流) | 高频参数更新 |
| `Command` | UI → C++ | JSON 字符串 | 低频 | 结构性指令（Undo/新建/删除等） |

---

## 1. `EQ_Curve_Data` — 频率响应曲线

**方向**: C++ → UI  
**触发**: UI 发送 View 边界后 C++ 重算  
**格式**: `Float32Array`，长度 = N × 2（N ≤ 150）

```
[freq1, gain1, freq2, gain2, ..., freqN, gainN]
```

| 字段 | 类型 | 范围 | 说明 |
|------|------|------|------|
| freq | float32 | 20.0 ~ 20000.0 | 频率 (Hz) |
| gain | float32 | -60.0 ~ +60.0 | 增益 (dB) |

**约束**:
- N ≤ 150（PRD 规格）
- freq 单调递增
- 基于当前 UI 视图窗口（`viewMinFreq` ~ `viewMaxFreq`）对数均匀采样
- 采样点为真实幅频响应极值点 + 拐点

**前端处理**:
- 接收后用 Catmull-Rom 样条插值补全像素级连续曲线
- **禁止**前端自行计算传递函数

**示例**（3 点）:
```
[100.0, 3.5, 500.0, 6.2, 2000.0, -1.8]
→ 100Hz +3.5dB, 500Hz +6.2dB, 2kHz -1.8dB
```

---

## 2. `Phase_Curve_Data` — 相位响应曲线

**方向**: C++ → UI  
**触发**: 同 EQ_Curve_Data  
**格式**: `Float32Array`，长度 = N × 2（N ≤ 150）

```
[freq1, phase1, freq2, phase2, ..., freqN, phaseN]
```

| 字段 | 类型 | 范围 | 说明 |
|------|------|------|------|
| freq | float32 | 20.0 ~ 20000.0 | 频率 (Hz) |
| phase | float32 | -180.0 ~ +180.0 | 相位 (度) |

**前端处理**: 叠加在 EQ 画布上，绿色半透明渲染。

---

## 3. `Spectrum_Data` — 实时频谱

**方向**: C++ → UI  
**触发**: 持续推送（~40Hz，约每 25ms 一次）  
**格式**: `Float32Array`，长度 = 512（对数频率点）

```
[pt0_dB, pt1_dB, pt2_dB, ..., pt511_dB]
```

| 字段 | 类型 | 范围 | 说明 |
|------|------|------|------|
| pt_dB | float32 | -120.0 ~ 0.0 | 该对数频点的 dBFS 幅值（已平滑） |

**约束**:
- 512 个点，**对数间隔**，point 0 = 20 Hz，point 511 = 20 kHz
- 底层 FFT size = 4096，Blackman-Harris 窗 + 相干增益校准
- **平滑在 C++ 侧完成**：分频带功率平均（频带半宽 `band`，低频不足 1 bin 时余弦插值）+ 弹道包络
- 参数宿主可调：attack 0.50 / release 0.96 / blur 0 / dilate 0 / band 0.02（octave）
- UI 只负责把 512 个点绘制成曲线（无需再采样/平滑）

**休眠**: UI 不可见时 C++ 停止推送。恢复可见后从下一个 hop 继续。

---

## 4. `Peak_Level` — 输出峰值电平

**方向**: C++ → UI  
**触发**: 每音频帧（~86Hz @ 512 samples buffer）  
**格式**: 单个 `Float32Array([peakDB])`

| 字段 | 类型 | 范围 | 说明 |
|------|------|------|------|
| peakDB | float32 | -∞ ~ 0.0 | 当前帧最大样本 dBFS |

**前端处理**:
- 更新峰值表数字显示（如 "-3.2 dB"）
- ≥ -0.1 dBFS：削波指示灯变红，保持 1 秒

---

## 5. `Parameter_Change` — 参数更新（UI → C++）

**方向**: UI → C++  
**触发**: 用户拖拽节点/旋钮/滑块时  
**格式**: `Float32Array([bandIndex, paramType, value])`

| 字段 | 类型 | 范围 | 说明 |
|------|------|------|------|
| bandIndex | float32 | -1 ~ 23 | Band 编号（-1 = 全局参数） |
| paramType | float32 | 0 ~ 8 | 参数类型枚举（见下表） |
| value | float32 | 依参数而定 | 新参数值 |

**paramType 枚举**:
| 值 | 含义 | value 范围 |
|----|------|-----------|
| 0 | Type (滤波器类型) | 0=Bell, 1=LowShelf, 2=HighShelf, 3=LowCut, 4=HighCut, 5=Notch |
| 1 | Frequency (Hz) | 20.0 ~ 20000.0 |
| 2 | Gain (dB) | -24.0 ~ +24.0 |
| 3 | Q | 0.1 ~ 40.0 |
| 4 | Active | 0.0 或 1.0 |
| 5 | Solo | 0.0 或 1.0 |
| 6 | Mute | 0.0 或 1.0 |
| 7 | Slope | 0=12, 1=24, 2=48, 3=96 |
| 8 | Output Gain (仅 bandIndex=-1) | -24.0 ~ +24.0 |

**节流规则**:
- 同一 `(bandIndex, paramType)` 组合的相邻两次发送间隔必须 ≥ 1ms
- 节流由**前端**负责（C++ 侧不做额外过滤）
- ⚠️ **DAW 自动化包络写入不受节流限制**（走独立 `Automation_Parameter` 通道，v1.1 实现）

**示例**（拖拽 Band 2 的 Gain 到 +5.0dB）:
```
[2.0, 2.0, 5.0]
→ bandIndex=2, paramType=2(Gain), value=5.0
```

---

## 6. `Command` — 结构性指令（UI → C++）

**方向**: UI → C++  
**触发**: 低频操作（新建节点、删除、Undo/Redo、预设等）  
**格式**: JSON 字符串

```json
{
  "command": "CommandName",
  "data": { ... }
}
```

**支持的命令**:

| command | data | 说明 |
|---------|------|------|
| `"NewBand"` | `{"freq": 1000, "gain": 0, "type": 0}` | 创建新 band |
| `"DeleteBand"` | `{"bandIndex": 3}` | 删除 band |
| `"CopyBand"` | `{"bandIndex": 2}` | 复制 band（C++ 返回 clipBoard 状态） |
| `"PasteBand"` | `{"freq": 1100}` | 粘贴 band 到指定频率 |
| `"Undo"` | `{}` | 撤销（C++ 侧维护栈） |
| `"Redo"` | `{}` | 重做 |
| `"SetViewRange"` | `{"minFreq": 20, "maxFreq": 20000}` | 通知 C++ 视图范围变化（50ms 防抖后发送） |
| `"SetAnalyzerActive"` | `{"active": true}` | 控制 FFT 线程休眠/唤醒 |
| `"LoadPreset"` | `{"json": "..."}` | 加载预设 JSON |
| `"SavePreset"` | `{"name": "MyPreset"}` | 保存预设（C++ 返回完整 JSON） |
| `"SetBypass"` | `{"bypass": true}` | 旁路开关 |
| `"SetOversample"` | `{"mode": 2}` | 0=Off, 1=2x, 2=4x |
| `"SetChannelMode"` | `{"mode": 0}` | 0=Stereo, 1=Left, 2=Right, 3=Mid, 4=Side |

---

## 7. UI 视图边界请求（特殊流程）

**流程**: UI 缩放/平移 → 50ms 防抖 → `Command{"SetViewRange"}` → C++ 重算 → `EQ_Curve_Data` + `Phase_Curve_Data` 推送

```
UI                           C++
 |                             |
 |-- Command("SetViewRange")-->|  (50ms debounce)
 |                             |  重算 150 点阵
 |<-- EQ_Curve_Data ----------|
 |<-- Phase_Curve_Data -------|
 |  Catmull-Rom 渲染           |
```

---

## 8. Mock 开发约定

在 C++ JSI 通道不可用期间，双方使用各自的 Mock 层独立开发：

**UI 侧 Mock** (`mockJSI.js`):
- `startMockSpectrum(onData)`: 每 ~46ms 推送随机频谱
- `getMockCurveData()`: 返回示例曲线点阵
- `getMockPeakLevel()`: 返回随机峰值
- 所有 `sendParameterChange` / `sendCommand` 调用打印到 console.log

**C++ 侧 Mock** (`MockAnalyzer.h`):
- 不依赖真实音频输入
- 生成正弦波 sweep 测试信号
- 验证 FFT 输出频谱 shape 正确

---

*文档版本: v1.0*
*生效日期: 2026-07-27*
*变更规则: 修改任何字段/枚举必须先更新本文档并通知对方 Agent*
