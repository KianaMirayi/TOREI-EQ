# TOREI-EQ v1.0 实现计划

> **For Hermes:** 使用 subagent-driven-development skill 按 Phase 逐任务实现。
> **最后更新：** 2026-07-27
> **关联文档：** `PRDV3.1.md`（需求真值源）

**目标：** 交付一款功能完整的 VST3/AU 静态 EQ 插件 — ZDF 引擎 + WebView2 UI + 频谱分析仪

**架构：** C++ JUCE 8 音频后端 + React/WebGL 前端 + JSI Float32Array 二进制通信桥

**技术栈：** C++17, JUCE 8 (MSVC), React 18, WebGL 2.0, Vite, CMake

---

## Phase 0: 项目脚手架与环境验证

> 目标：确保 JUCE + WebView2 + React 模板跑通，不涉及任何 DSP

### Task 0.1: 创建 JUCE CMake 项目骨架

**目标：** 生成能编译通过的空白 VST3 插件，验证 MSVC 工具链

**文件：**
- 创建: `CMakeLists.txt`
- 创建: `src/PluginEditor.h`, `src/PluginEditor.cpp`
- 创建: `src/PluginProcessor.h`, `src/PluginProcessor.cpp`

**Step 1: 创建 CMakeLists.txt**

```cmake
cmake_minimum_required(VERSION 3.22)
project(TOREI-EQ VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE 路径 — 环境变量或硬编码
set(JUCE_ROOT "E:/Programing/C++Hell/JUCE" CACHE PATH "JUCE root directory")

add_subdirectory(${JUCE_ROOT} juce_build)

juce_add_plugin(TOREI-EQ
    COMPANY_NAME "TOREI"
    PLUGIN_NAME "TOREI-EQ"
    PRODUCT_NAME "TOREI-EQ"
    VERSION "1.0.0"
    FORMATS VST3 Standalone
    PLUGIN_MANUFACTURER_CODE Tore
    PLUGIN_CODE Eq00
    
    SOURCES
        src/PluginProcessor.cpp
        src/PluginEditor.cpp
)
```

**Step 2: 创建 PluginProcessor.h/cpp（最小实现）**

`src/PluginProcessor.h`:
```cpp
#pragma once
#include <JuceHeader.h>

class ToreiEQAudioProcessor : public juce::AudioProcessor
{
public:
    ToreiEQAudioProcessor();
    ~ToreiEQAudioProcessor() override;
    
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    
    const juce::String getName() const override { return "TOREI-EQ"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ToreiEQAudioProcessor)
};
```

`src/PluginProcessor.cpp`:
```cpp
#include "PluginProcessor.h"
#include "PluginEditor.h"

ToreiEQAudioProcessor::ToreiEQAudioProcessor()
    : juce::AudioProcessor(BusesProperties()
        .withInput("Input", juce::AudioChannelSet::stereo(), true)
        .withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
}

ToreiEQAudioProcessor::~ToreiEQAudioProcessor() {}

void ToreiEQAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused(sampleRate, samplesPerBlock);
}

void ToreiEQAudioProcessor::releaseResources() {}

void ToreiEQAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    // Passthrough for now
}

juce::AudioProcessorEditor* ToreiEQAudioProcessor::createEditor()
{
    return new ToreiEQAudioProcessorEditor(*this);
}
```

**Step 3: 创建 PluginEditor.h/cpp（最小实现）**

```cpp
#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"

class ToreiEQAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    explicit ToreiEQAudioProcessorEditor(ToreiEQAudioProcessor&);
    ~ToreiEQAudioProcessorEditor() override;
    void paint(juce::Graphics&) override;
    void resized() override;
private:
    ToreiEQAudioProcessor& processorRef;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ToreiEQAudioProcessorEditor)
};
```

**Step 4: 编译验证**

```bash
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Debug
```

**预期输出：** 生成 `TOREI-EQ.vst3` 插件文件，在 DAW 中可加载并显示空白界面。

---

### Task 0.2: WebView2 React 前端项目搭建

**目标：** 创建独立 React 前端项目，WebGL Canvas 能画出一条静态曲线

**文件：**
- 创建: `ui/package.json`
- 创建: `ui/vite.config.js`
- 创建: `ui/src/main.jsx`
- 创建: `ui/src/App.jsx`
- 创建: `ui/src/canvas/EQCurveCanvas.jsx`

**Step 1: 初始化 Vite + React 项目**

```bash
cd ui
npm create vite@latest . -- --template react
npm install
```

**Step 2: 创建 EQCurveCanvas 组件**

`ui/src/canvas/EQCurveCanvas.jsx`:
```jsx
import { useRef, useEffect, useCallback } from 'react';

// 硬编码测试曲线数据（后续替换为 JSI Float32Array）
const TEST_POINTS = [
  { freq: 20, gain: 0 }, { freq: 50, gain: 0 }, { freq: 100, gain: 2 },
  { freq: 200, gain: 5 }, { freq: 500, gain: 8 }, { freq: 1000, gain: 6 },
  { freq: 2000, gain: 3 }, { freq: 5000, gain: 0 }, { freq: 10000, gain: -2 },
  { freq: 20000, gain: 0 },
];

function freqToX(freq, width) {
  const minLog = Math.log10(20);
  const maxLog = Math.log10(20000);
  return ((Math.log10(freq) - minLog) / (maxLog - minLog)) * width;
}

function gainToY(gain, height) {
  return height / 2 - (gain / 30) * (height / 2);
}

export default function EQCurveCanvas({ points = TEST_POINTS }) {
  const canvasRef = useRef(null);
  
  const draw = useCallback(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const ctx = canvas.getContext('2d');
    const dpr = window.devicePixelRatio || 1;
    const rect = canvas.getBoundingClientRect();
    canvas.width = rect.width * dpr;
    canvas.height = rect.height * dpr;
    ctx.scale(dpr, dpr);
    
    const { width, height } = rect;
    ctx.clearRect(0, 0, width, height);
    
    // Draw grid
    ctx.strokeStyle = 'rgba(255,255,255,0.08)';
    ctx.lineWidth = 1;
    [20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000].forEach(f => {
      const x = freqToX(f, width);
      ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, height); ctx.stroke();
    });
    [-24, -12, 0, 12, 24].forEach(g => {
      const y = gainToY(g, height);
      ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(width, y); ctx.stroke();
    });
    
    // Draw curve (linear interpolation for now, Catmull-Rom later)
    ctx.strokeStyle = '#4FC3F7';
    ctx.lineWidth = 2;
    ctx.beginPath();
    points.forEach((p, i) => {
      const x = freqToX(p.freq, width);
      const y = gainToY(p.gain, height);
      if (i === 0) ctx.moveTo(x, y);
      else ctx.lineTo(x, y);
    });
    ctx.stroke();
  }, [points]);
  
  useEffect(() => { draw(); }, [draw]);
  
  return (
    <canvas ref={canvasRef}
      style={{ width: '100%', height: '100%', background: '#1a1a2e' }}
    />
  );
}
```

**Step 3: 验证构建产物体积**

```bash
npm run build
ls -lh dist/assets/*.js   # 必须 < 500KB（后续增长空间留给频谱渲染）
```

**预期输出：** 打开 `http://localhost:5173`，看到深色背景 + 白色网格 + 蓝色 EQ 曲线。

---

### Task 0.3: WebView2 接入 JUCE 插件

**目标：** JUCE 插件界面显示 React dev server 页面

**文件：**
- 修改: `src/PluginEditor.h`, `src/PluginEditor.cpp`
- 修改: `CMakeLists.txt`

**Step 1: 修改 CMakeLists.txt 启用 WebView2**

```cmake
target_compile_definitions(TOREI-EQ PUBLIC
    JUCE_WEB_BROWSER=1
    JUCE_USE_WIN_WEBVIEW2=1
)
```

**Step 2: 修改 PluginEditor.h 加入 WebBrowserComponent**

```cpp
#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"

class ToreiEQAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    explicit ToreiEQAudioProcessorEditor(ToreiEQAudioProcessor&);
    ~ToreiEQAudioProcessorEditor() override;
    void resized() override;
    
private:
    ToreiEQAudioProcessor& processorRef;
    juce::WebBrowserComponent webView;
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ToreiEQAudioProcessorEditor)
};
```

**Step 3: 修改 PluginEditor.cpp**

```cpp
#include "PluginEditor.h"

ToreiEQAudioProcessorEditor::ToreiEQAudioProcessorEditor(ToreiEQAudioProcessor& p)
    : AudioProcessorEditor(&p), processorRef(p)
{
    // JUCE 8 WebView2 — 必须 setBounds 在 addAndMakeVisible 之前
    webView.setBounds(getLocalBounds());
    
    // 开发阶段指向 Vite dev server
    webView.goToURL("http://localhost:5173");
    
    addAndMakeVisible(webView);
    setSize(900, 600);
    setResizable(true, true);
}

ToreiEQAudioProcessorEditor::~ToreiEQAudioProcessorEditor() {}

void ToreiEQAudioProcessorEditor::resized()
{
    webView.setBounds(getLocalBounds());
}
```

**Step 4: 编译并在 DAW 中加载**

```bash
cmake --build build --config Debug
```

**预期输出：** 插件窗口出现后 2-3 秒内显示 React dev server 的 EQ 曲线画布。

**无头规避（PRD §7.2）：**

```cpp
// PluginProcessor.cpp
ToreiEQAudioProcessor::ToreiEQAudioProcessor()
{
    // 离线渲染/无头扫描时跳过 WebView
    if (juce::PluginHostType::getPluginLoadedAs() == juce::AudioProcessor::wrapperType_Undefined)
        return;
}
```

---

## Phase 1: ZDF 静态 EQ 引擎

> 目标：完整的 IIR ZDF DSP 核心 — Bell, Shelf, Cut, Notch，参数平滑，系数计算

### Task 1.1: 创建 APVTS 参数树

**目标：** 定义所有 v1.0 参数的 AudioProcessorValueTreeState

**文件：**
- 创建: `src/dsp/Parameters.h`
- 修改: `src/PluginProcessor.h`

**Step 1: Parameters.h**

```cpp
#pragma once
#include <JuceHeader.h>

namespace ToreiParams
{
    constexpr int MAX_BANDS = 24;
    
    // Per-band parameter IDs
    inline juce::String bandID(int band, const juce::String& param)
    {
        return "band_" + juce::String(band) + "_" + param;
    }
    
    enum class FilterType : int
    {
        Bell = 0, LowShelf, HighShelf, LowCut, HighCut, Notch
    };
    
    enum class ChannelMode : int
    {
        Stereo = 0, Left, Right, Mid, Side
    };
    
    // Global parameter IDs
    const juce::String PID_OUTPUT_GAIN  = "output_gain";
    const juce::String PID_BYPASS       = "bypass";
    const juce::String PID_CHANNEL_MODE = "channel_mode";
    const juce::String PID_CHANNEL_LINK = "channel_link";
    const juce::String PID_OVERSAMPLE   = "oversample";
    const juce::String PID_AUTO_GAIN    = "auto_gain";
    
    // Per-band parameter IDs
    inline juce::String pidType(int b)     { return bandID(b, "type"); }
    inline juce::String pidActive(int b)   { return bandID(b, "active"); }
    inline juce::String pidFreq(int b)     { return bandID(b, "freq"); }
    inline juce::String pidGain(int b)     { return bandID(b, "gain"); }
    inline juce::String pidQ(int b)        { return bandID(b, "q"); }
    inline juce::String pidSlope(int b)    { return bandID(b, "slope"); }
    inline juce::String pidSolo(int b)     { return bandID(b, "solo"); }
    inline juce::String pidMute(int b)     { return bandID(b, "mute"); }
    
    inline juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
    {
        juce::AudioProcessorValueTreeState::ParameterLayout layout;
        
        // Global params
        layout.add(std::make_unique<juce::AudioParameterFloat>(
            PID_OUTPUT_GAIN, "Output Gain", -24.0f, 24.0f, 0.0f));
        layout.add(std::make_unique<juce::AudioParameterBool>(
            PID_BYPASS, "Bypass", false));
        layout.add(std::make_unique<juce::AudioParameterChoice>(
            PID_CHANNEL_MODE, "Channel Mode",
            juce::StringArray{"Stereo", "Left", "Right", "Mid", "Side"}, 0));
        layout.add(std::make_unique<juce::AudioParameterBool>(
            PID_CHANNEL_LINK, "Channel Link", false));
        layout.add(std::make_unique<juce::AudioParameterChoice>(
            PID_OVERSAMPLE, "Oversampling",
            juce::StringArray{"Off", "2x", "4x"}, 0));
        layout.add(std::make_unique<juce::AudioParameterBool>(
            PID_AUTO_GAIN, "Auto Gain", false));
        
        // 24 bands
        for (int b = 0; b < MAX_BANDS; ++b)
        {
            layout.add(std::make_unique<juce::AudioParameterChoice>(
                pidType(b), "Band " + juce::String(b) + " Type",
                juce::StringArray{"Bell", "LowShelf", "HighShelf", "LowCut", "HighCut", "Notch"}, 0));
            layout.add(std::make_unique<juce::AudioParameterBool>(
                pidActive(b), "Band " + juce::String(b) + " Active", b == 0)); // Band 0 default active
            layout.add(std::make_unique<juce::AudioParameterFloat>(
                pidFreq(b), "Band " + juce::String(b) + " Freq",
                juce::NormalisableRange<float>(20.0f, 20000.0f, 1.0f, 0.3f), 1000.0f));
            layout.add(std::make_unique<juce::AudioParameterFloat>(
                pidGain(b), "Band " + juce::String(b) + " Gain",
                -24.0f, 24.0f, 0.0f));
            layout.add(std::make_unique<juce::AudioParameterFloat>(
                pidQ(b), "Band " + juce::String(b) + " Q",
                0.1f, 40.0f, 1.0f));
            layout.add(std::make_unique<juce::AudioParameterChoice>(
                pidSlope(b), "Band " + juce::String(b) + " Slope",
                juce::StringArray{"12", "24", "48", "96"}, 1));
            layout.add(std::make_unique<juce::AudioParameterBool>(
                pidSolo(b), "Band " + juce::String(b) + " Solo", false));
            layout.add(std::make_unique<juce::AudioParameterBool>(
                pidMute(b), "Band " + juce::String(b) + " Mute", false));
        }
        
        return layout;
    }
}
```

**验证：** 在 PluginProcessor 构造函数中创建 APVTS，编译通过。

---

### Task 1.2: ZDF 滤波器系数计算

**目标：** 实现 Bell/Shelf/Cut/Notch 的 IIR 系数计算（ZDF TPT 结构）

**文件：**
- 创建: `src/dsp/ZDFCoefficients.h`
- 创建: `src/dsp/ZDFCoefficients.cpp`

**系数计算参考**：使用 JUCE 的 `juce::dsp::IIR::Coefficients` 作为验证基准，但自行实现 ZDF/TPT 版本。

```cpp
// ZDFCoefficients.h
#pragma once
#include <JuceHeader.h>

struct ZDFCoefficients
{
    float b0, b1, b2;  // feed-forward
    float a1, a2;      // feedback (a0 = 1.0 in TPT form)
    
    static ZDFCoefficients makeBell(float freq, float Q, float gainDB, double sampleRate);
    static ZDFCoefficients makeLowShelf(float freq, float Q, float gainDB, double sampleRate);
    static ZDFCoefficients makeHighShelf(float freq, float Q, float gainDB, double sampleRate);
    static ZDFCoefficients makeLowCut(float freq, float Q, int order, double sampleRate);
    static ZDFCoefficients makeHighCut(float freq, float Q, int order, double sampleRate);
    static ZDFCoefficients makeNotch(float freq, float Q, double sampleRate);
    
    // Helper: pre-warp + BLT
    static float preWarp(float freq, double sampleRate);
};

// ZDFCoefficients.cpp — 关键实现

float ZDFCoefficients::preWarp(float freq, double sampleRate)
{
    // 使用 std::tan — 系数更新路径频率极低，允许
    return std::tan(juce::MathConstants<float>::pi * freq / static_cast<float>(sampleRate));
}

ZDFCoefficients ZDFCoefficients::makeBell(float freq, float Q, float gainDB, double sampleRate)
{
    // Analog prototype → BLT → TPT coefficients
    // 参考: V. Zavalishin "The Art of VA Filter Design"
    const float w0 = preWarp(freq, sampleRate);
    const float A  = std::pow(10.0f, gainDB / 40.0f);  // sqrt of linear gain
    
    const float alpha = w0 / (2.0f * Q);
    
    // Biquad coefficients (cookbook form)
    const float b0 = 1.0f + alpha * A;
    const float b1 = -2.0f * (1.0f - w0 * w0) / (1.0f + alpha / A); // simplified
    // ... 完整实现见 JUCE dsp::IIR::Coefficients 源码
}
```

**⚠️ 注意事项：**
1. 系数更新路径（`makeBell` 等）允许直接使用 `std::tan`/`std::pow` — 仅用户调参时触发
2. `processBlock` 热路径只有乘加运算，零超越函数
3. 以 JUCE 内置 `juce::dsp::IIR::Coefficients` 作为单元测试验证基准

**验证：**
```cpp
// 单元测试伪码
auto expected = juce::dsp::IIR::Coefficients<float>::makePeakFilter(44100, 1000, 1.0, 6.0);
auto actual   = ZDFCoefficients::makeBell(1000, 1.0, 6.0, 44100);
// 验证 b0/b1/b2/a1/a2 误差 < 1e-4
```

---

### Task 1.3: ZDF 滤波器结构体（per-band processor）

**目标：** 每个 band 持有两个 ZDF 二阶节（stereo），支持参数平滑

**文件：**
- 创建: `src/dsp/ZDFFilter.h`
- 创建: `src/dsp/ZDFFilter.cpp`

```cpp
// ZDFFilter.h
#pragma once
#include "ZDFCoefficients.h"

class ZDFFilter
{
public:
    void prepare(double sampleRate);
    void reset();
    
    // 更新系数（触发于参数变更）— 允许超越函数
    void updateCoefficients(const ZDFCoefficients& coeffs);
    
    // 热路径处理 — 零超越函数
    void processSample(float& leftSample, float& rightSample) noexcept;
    
    // 参数平滑（de-zipper）
    void setGainTarget(float gainDB);
    void smoothParameters();

private:
    // TPT 状态变量
    struct ChannelState { float s1 = 0, s2 = 0; };
    ChannelState left, right;
    
    // 当前系数
    float b0 = 1, b1 = 0, b2 = 0;
    float a1 = 0, a2 = 0;
    
    // 参数平滑
    float currentGain = 0;
    float targetGain = 0;
    
    double sampleRate = 44100;
};
```

**热路径实现：**
```cpp
void ZDFFilter::processSample(float& L, float& R) noexcept
{
    // TPT 结构 — 零分配，零超越函数
    // y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]
    
    const float Lout = b0 * L + b1 * left.x1 + b2 * left.x2
                     - a1 * left.y1 - a2 * left.y2;
    left.x2 = left.x1; left.x1 = L;
    left.y2 = left.y1; left.y1 = Lout;
    L = Lout;
    
    const float Rout = b0 * R + b1 * right.x1 + b2 * right.x2
                      - a1 * right.y1 - a2 * right.y2;
    right.x2 = right.x1; right.x1 = R;
    right.y2 = right.y1; right.y1 = Rout;
    R = Rout;
}
```

---

### Task 1.4: 完整 EQ 处理链（processBlock 主流程）

**目标：** 将所有 band 串联 + 通道路由 + Solo/Mute + 旁路 + Auto-Gain

**文件：**
- 创建: `src/dsp/EQEngine.h`
- 创建: `src/dsp/EQEngine.cpp`
- 修改: `src/PluginProcessor.cpp`

```cpp
// EQEngine.h
#pragma once
#include "ZDFFilter.h"
#include "Parameters.h"
#include <array>

class EQEngine
{
public:
    void prepare(double sampleRate, int maxBlockSize);
    void reset();
    void processBlock(juce::AudioBuffer<float>& buffer,
                      const juce::AudioProcessorValueTreeState& apvts);
    
    // 获取当前 RMS（用于 Auto-Gain）
    float getInputRMS() const  { return inputRMS; }
    float getOutputRMS() const { return outputRMS; }
    
    // 获取峰值（用于峰值表）
    float getPeakLevel() const { return maxPeak; }

private:
    std::array<ZDFFilter, ToreiParams::MAX_BANDS> bands;
    
    // 平滑缓冲区（用于 bypass crossfade + parameter de-zipper）
    juce::AudioBuffer<float> dryBuffer;
    
    // 统计
    float inputRMS = 0, outputRMS = 0;
    float maxPeak = 0;
    
    double sampleRate = 44100;
    
    // 旁路交叉淡入淡出状态
    float bypassFade = 0; // 0 = dry, 1 = wet
    bool bypassTarget = false;
    static constexpr int FADE_SAMPLES = 2205; // 50ms @ 44.1k
};
```

**processBlock 伪代码：**
```cpp
void EQEngine::processBlock(juce::AudioBuffer<float>& buffer, const APVTS& apvts)
{
    // 1. 测量输入 RMS
    inputRMS = buffer.getRMSLevel(0, 0, buffer.getNumSamples());
    
    // 2. 复制 dry 信号（用于 bypass crossfade 和 channel 路由）
    dryBuffer.makeCopyOf(buffer, true);
    
    // 3. 逐 band 处理
    bool anySolo = /* 检查是否有任何 band solo */;
    
    for (int b = 0; b < ToreiParams::MAX_BANDS; ++b)
    {
        if (!isActive[b]) continue;
        if (anySolo && !isSolo[b]) continue;
        if (isMute[b]) continue;
        
        // 更新系数（如果参数变化）
        if (paramChanged[b])
            bands[b].updateCoefficients(computeZDF(b));
        
        // 处理全部采样点
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            auto* data = buffer.getWritePointer(ch);
            for (int s = 0; s < buffer.getNumSamples(); ++s)
                bands[b].processSample(data[s]); // Mono per-channel
        }
    }
    
    // 4. 输出增益
    buffer.applyGain(juce::Decibels::decibelsToGain(outputGainDB));
    
    // 5. Smart Bypass crossfade（50ms）
    if (bypassTarget != (bypassFade > 0.5f))
    { /* 交叉淡入淡出逻辑 */ }
    
    // 6. 测量输出 RMS + 峰值
    outputRMS = buffer.getRMSLevel(0, 0, buffer.getNumSamples());
    maxPeak = buffer.getMagnitude(0, 0, buffer.getNumSamples()); // max |sample|
    
    // 7. Auto-Gain
    if (autoGainOn)
        buffer.applyGain(inputRMS / outputRMS);
}
```

**验证：** 在 DAW 中插入插件，修改 Band 0 的 Gain，听感确认 EQ 生效。

---

## Phase 2: FFT 频谱分析线程

> 目标：独立高优先级 std::thread 执行 8192-pt FFT，SPSC 无锁队列传数据

### Task 2.1: SPSC 环形缓冲区

**文件：**
- 创建: `src/dsp/SPSCQueue.h`

```cpp
// 无锁单生产者单消费者队列（lock-free SPSC）
template<typename T, size_t Capacity>
class SPSCQueue
{
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
    
    std::array<T, Capacity> buffer;
    std::atomic<size_t> writeIdx{0};
    std::atomic<size_t> readIdx{0};
    
    static constexpr size_t mask = Capacity - 1;
    
public:
    bool push(const T& item)
    {
        size_t w = writeIdx.load(std::memory_order_relaxed);
        if (w - readIdx.load(std::memory_order_acquire) >= Capacity)
            return false; // full
        buffer[w & mask] = item;
        writeIdx.store(w + 1, std::memory_order_release);
        return true;
    }
    
    bool pop(T& item)
    {
        size_t r = readIdx.load(std::memory_order_relaxed);
        if (r == writeIdx.load(std::memory_order_acquire))
            return false; // empty
        item = buffer[r & mask];
        readIdx.store(r + 1, std::memory_order_release);
        return true;
    }
    
    bool empty() const
    {
        return readIdx.load(std::memory_order_acquire) ==
               writeIdx.load(std::memory_order_acquire);
    }
};
```

---

### Task 2.2: FFT 分析线程

**目标：** 独立线程采集音频 → FFT → 平滑 → Triple Buffer 推送

**文件：**
- 创建: `src/dsp/AnalyzerThread.h`
- 创建: `src/dsp/AnalyzerThread.cpp`
- 创建: `src/dsp/TripleBuffer.h`

```cpp
// AnalyzerThread.h
#pragma once
#include "SPSCQueue.h"
#include "TripleBuffer.h"

class AnalyzerThread
{
public:
    AnalyzerThread();
    ~AnalyzerThread();
    
    void start(double sampleRate);
    void stop();
    
    // processBlock 调用 — 仅拷贝数据到 SPSC Queue
    void pushAudioBlock(const float* left, const float* right, int numSamples) noexcept;
    
    // UI 调用 — 获取最新频谱数据
    bool getSpectrumData(std::vector<float>& outData);
    
    // UI 调用 — 获取峰值电平
    float getPeakLevel() const;
    
    void setActive(bool active);
    
private:
    void run(); // 线程主循环
    
    struct AudioChunk
    {
        static constexpr int MAX_SAMPLES = 512;
        float left[MAX_SAMPLES];
        float right[MAX_SAMPLES];
        int count;
    };
    
    SPSCQueue<AudioChunk, 64> audioQueue;
    TripleBuffer<std::vector<float>> spectrumBuffer;
    
    std::unique_ptr<std::thread> worker;
    std::atomic<bool> running{false};
    std::atomic<bool> active{true}; // 休眠标志
    
    double sampleRate = 44100;
    std::atomic<float> currentPeak{0};
    
    // FFT workspace
    std::vector<float> fftBuffer;  // 8192 samples
    std::vector<float> window;     // Blackman-Harris precomputed
    int fftWritePos = 0;           // 环形写入位置
    int overlapCounter = 0;
    static constexpr int FFT_SIZE = 8192;
    static constexpr int HOP_SIZE = 2048; // 75% overlap = 8192 / 4
    static constexpr float EMA_ALPHA = 0.01f; // ~100ms @ 46ms hop
};
```

**run() 核心循环：**
```cpp
void AnalyzerThread::run()
{
    // 预计算 Blackman-Harris 窗
    for (int i = 0; i < FFT_SIZE; ++i)
        window[i] = /* Blackman-Harris formula */;
    
    while (running)
    {
        if (!active)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        
        AudioChunk chunk;
        if (!audioQueue.pop(chunk))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        
        // 拷贝到 FFT 环形缓冲区
        for (int i = 0; i < chunk.count; ++i)
        {
            fftBuffer[fftWritePos] = (chunk.left[i] + chunk.right[i]) * 0.5f;
            fftWritePos = (fftWritePos + 1) % FFT_SIZE;
        }
        
        ++overlapCounter;
        if (overlapCounter * chunk.count >= HOP_SIZE)
        {
            overlapCounter = 0;
            
            // 执行 FFT
            // 1. Apply window
            // 2. Real FFT (可以用 JUCE::dsp::FFT 或 pffft)
            // 3. Magnitude spectrum → dB
            // 4. EMA 平滑
            // 5. 写入 TripleBuffer
        }
    }
}
```

**验证：** 在玩具音频源下运行，打印频谱数据确认 FFT bins 有意义。

---

## Phase 3: JSI 通信桥

> 目标：C++ ↔ JavaScript 双向 Float32Array 通信，节流 + 防抖

### Task 3.1: JSI Bridge（C++ 侧）

**文件：**
- 创建: `src/bridge/JSIBridge.h`
- 创建: `src/bridge/JSIBridge.cpp`

```cpp
// JSIBridge.h
#pragma once
#include <JuceHeader.h>

class JSIBridge : private juce::WebBrowserComponent::Options::NativeFunctionHandler
{
public:
    JSIBridge(juce::WebBrowserComponent& webView,
              EQEngine& engine, AnalyzerThread& analyzer);
    
    // C++ → JS: push curve data
    void pushCurveData(const juce::Array<juce::Point<float>>& points);
    
    // C++ → JS: push spectrum data
    void pushSpectrumData(const std::vector<float>& bins);
    
    // C++ → JS: push peak meter
    void pushPeakLevel(float peak);
    
    // JS → C++: handle incoming messages
    void handleNativeFunction(const juce::String& name,
                              const juce::var::NativeFunctionArgs& args) override;

private:
    juce::WebBrowserComponent& webView;
    EQEngine& engine;
    AnalyzerThread& analyzer;
    
    // 节流: UI→Audio 指令间隔 >= 1ms
    juce::Time lastUICommand;
};
```

---

### Task 3.2: JSI Adapter（UI 侧）

**文件：**
- 创建: `ui/src/bridge/jsiAdapter.js`
- 创建: `ui/src/bridge/mockJSI.js` (开发用假数据)

```js
// jsiAdapter.js
// 生产环境: 通过 window.__juce__ API 与 C++ 通信
// 开发环境: 使用 MockJSI 提供假数据

const isJUCE = () => typeof window.__juce__ !== 'undefined';

class JSIAdapter {
  constructor() {
    this.callbacks = {
      curveData: [],
      spectrumData: [],
      peakLevel: [],
    };
    this.throttleTimers = {};
    this.setup();
  }
  
  setup() {
    if (isJUCE()) {
      // 生产: 注册 C++ → JS 回调
      window.__juce__.on('EQ_Curve_Data', data => {
        this.emit('curveData', new Float32Array(data));
      });
      window.__juce__.on('Spectrum_Data', data => {
        this.emit('spectrumData', new Float32Array(data));
      });
      window.__juce__.on('Peak_Level', level => {
        this.emit('peakLevel', level);
      });
    }
  }
  
  // JS → C++: 参数变更（带 1ms 节流）
  sendParameterChange(bandIndex, paramType, value) {
    const key = `${bandIndex}_${paramType}`;
    const now = performance.now();
    
    if (this.throttleTimers[key] && now - this.throttleTimers[key] < 1) {
      return; // 丢弃间隔 < 1ms 的指令
    }
    this.throttleTimers[key] = now;
    
    if (isJUCE()) {
      window.__juce__.postMessage('Parameter_Change',
        new Float32Array([bandIndex, paramType, value]));
    }
  }
  
  // JS → C++: 结构性指令（Undo/Redo/NewBand 等）
  sendCommand(command, data = {}) {
    if (isJUCE()) {
      window.__juce__.postMessage('Command', JSON.stringify({ command, ...data }));
    }
  }
  
  // 事件系统
  on(event, callback) { this.callbacks[event].push(callback); }
  emit(event, data) { this.callbacks[event].forEach(cb => cb(data)); }
}
```

```js
// mockJSI.js — 开发阶段提供假频谱数据
export class MockJSI {
  constructor() {
    this.curvePoints = this.generateCurvePoints();
    this.spectrumBins = new Float32Array(4096);
    this.rafId = null;
  }
  
  generateCurvePoints() {
    // 生成 150 点贝塞尔曲线测试数据
    const points = new Float32Array(300);
    for (let i = 0; i < 150; i++) {
      const freq = 20 * Math.pow(10, (i / 149) * 3);
      points[i * 2] = freq;
      points[i * 2 + 1] = Math.sin(i / 20) * 6;
    }
    return points;
  }
  
  startMockSpectrum(onData) {
    const animate = () => {
      for (let i = 0; i < 4096; i++) {
        this.spectrumBins[i] = Math.random() * -60 - 20;
      }
      onData(this.spectrumBins);
      this.rafId = requestAnimationFrame(animate);
    };
    animate();
  }
  
  stop() { cancelAnimationFrame(this.rafId); }
}
```

---

## Phase 4: UI 核心渲染

> 目标：完整 EQ 界面 — 曲线编辑、节点交互、频谱、控制面板

### Task 4.1: WebGL 曲线渲染器（Catmull-Rom 插值）

**文件：**
- 创建: `ui/src/canvas/CurveRenderer.js`

```js
// Catmull-Rom 样条插值
function catmullRom(p0, p1, p2, p3, t) {
  const t2 = t * t, t3 = t2 * t;
  return {
    x: 0.5 * ((2*p1.x) + (-p0.x + p2.x)*t + (2*p0.x - 5*p1.x + 4*p2.x - p3.x)*t2 + (-p0.x + 3*p1.x - 3*p2.x + p3.x)*t3),
    y: 0.5 * ((2*p1.y) + (-p0.y + p2.y)*t + (2*p0.y - 5*p1.y + 4*p2.y - p3.y)*t2 + (-p0.y + 3*p1.y - 3*p2.y + p3.y)*t3)
  };
}
```

在 WebGL shader 中实现或预计算为顶点缓冲上传。

---

### Task 4.2: 节点交互系统

**目标：** 拖拽节点控制 Gain/Freq/Q，双击创建/删除，右键菜单

**文件：**
- 创建: `ui/src/canvas/NodeInteraction.js`
- 创建: `ui/src/components/BandNode.jsx`
- 创建: `ui/src/components/ContextMenu.jsx`

**交互流程：**
```
mousedown on node   → 开始拖拽
mousemove           → 更新 Gain/Freq/Q + 节流发送 JSI
mouseup             → 停止拖拽，触发 Undo 快照
dblclick on node    → 删除节点
dblclick on canvas  → 创建节点
rightclick on node  → ContextMenu (类型切换/复制/粘贴/删除)
Delete key          → 删除选中节点
```

---

### Task 4.3: 频谱分析仪 WebGL 渲染

**文件：**
- 创建: `ui/src/canvas/SpectrumRenderer.js`

使用 `gl.TRIANGLE_STRIP` 绘制填充频谱，每帧更新顶点缓冲。

```js
// Spectrum vertex shader
const vertShader = `
  attribute vec2 a_position;
  void main() {
    gl_Position = vec4(a_position, 0.0, 1.0);
  }
`;

const fragShader = `
  precision mediump float;
  uniform vec4 u_colorBottom;
  uniform vec4 u_colorTop;
  varying float v_gradient;
  void main() {
    gl_FragColor = mix(u_colorBottom, u_colorTop, v_gradient);
  }
`;
```

---

### Task 4.4: 控制面板组件

**文件：**
- 创建: `ui/src/components/Knob.jsx`       (旋钮组件)
- 创建: `ui/src/components/Slider.jsx`     (滑块组件)
- 创建: `ui/src/components/Toggle.jsx`     (开关组件)
- 创建: `ui/src/components/Dropdown.jsx`   (下拉菜单)
- 创建: `ui/src/components/PeakMeter.jsx`  (峰值表 + 削波灯)
- 创建: `ui/src/components/BottomBar.jsx`  (底部全局控制)
- 创建: `ui/src/components/RightPanel.jsx` (右侧频段参数)

**PeakMeter 规格：**
- 纵向电平条（绿色 -20~0, 黄色 -6~0, 红色 >0）
- 峰值数字显示（如 "-3.2 dB"）
- 削波指示灯：≥ -0.1 dBFS 保持红色 1 秒
- 数据源：JSI `Peak_Level` 通道

---

### Task 4.5: Phase Display 渲染

**文件：**
- 创建: `ui/src/canvas/PhaseRenderer.js`

在 EQ 曲线画布上叠加第二组曲线数据，绿色半透明，Y 轴范围 -180° ~ +180°。

---

### Task 4.6: 预设管理 UI

**文件：**
- 创建: `ui/src/components/PresetBar.jsx`
- 创建: `ui/src/components/PresetManager.jsx`

- 预设下拉列表
- Save / Save As / Delete
- "Copy as Base64" 按钮 → `navigator.clipboard.writeText()`
- "Paste from Base64" → 解码 → 应用

---

## Phase 5: 集成联调

### Task 5.1: 生产构建打包

**目标：** Vite 构建产物嵌入 JUCE BinaryData

```cmake
# CMakeLists.txt
juce_add_binary_data(TOREI-EQ_UIData
    SOURCES ui/dist/index.html ui/dist/assets/...)
target_link_libraries(TOREI-EQ PRIVATE TOREI-EQ_UIData)
```

开发阶段：WebView 指向 `http://localhost:5173`  
发布阶段：WebView 加载内嵌 `index.html`

---

### Task 5.2: 完整集成测试

- [ ] 插件在 REAPER / Studio One 中加载
- [ ] 添加 5 个 band 并拖拽调整 → 听感正确
- [ ] Solo/Mute → 行为正确
- [ ] Bypass crossfade → 无爆音
- [ ] 频谱显示 → 与音频输入匹配
- [ ] 预设保存/加载 → 参数完整恢复
- [ ] Undo/Redo → 状态回滚正确
- [ ] 多实例（8 个）→ 无崩溃
- [ ] 关闭 UI → FFT 线程休眠

---

## 开发顺序总结

```
Phase 0: 脚手架            (1-2 天)  ← 你现在在这里
Phase 1: DSP 引擎          (2-3 天)
Phase 2: FFT 分析线程      (2-3 天)
Phase 3: JSI 桥梁          (2-3 天)
Phase 4: UI 渲染           (3-5 天)
Phase 5: 集成联调           (3-5 天)
────────────────────────────────────
总计：                      13-21 天
缓冲 + 测试：               +1 周
目标 v1.0 发布：            ~4 周
```

---

*计划版本: v1.0*
*下次更新触发: Phase 1 开始前技术选型确认*
