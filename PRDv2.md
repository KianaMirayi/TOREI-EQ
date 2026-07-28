# 🎛️ 现代化高阶数字均衡器插件（对标 Pro-Q 3）- 产品需求文档 (PRD) v2.0

## 1. 项目概述与核心愿景

本产品是一款面向专业混音与母带工程师的高性能数字均衡器（EQ）插件。产品在视觉与交互体验上全面对标行业标杆 **FabFilter Pro-Q 3**。通过“C++ 极致 DSP 引擎 + 系统原生 Web UI”的混合架构，在提供 0 延迟、工业级纯净音质的同时，实现跨时代丝滑的现代化动态视觉交互。

---

## 2. 核心功能需求 (Must-Have)

### 2.1 动态均衡处理 (Dynamic EQ)
*   **触发机制**：支持内部（RMS/Peak 检测）及外部 DAW 侧链（Sidechain）输入触发。
*   **动态模式**：必须同时支持“向下压缩（Downward Compression）”与“向上扩展（Upward Expansion）”。
*   **时域控制**：提供可调的 Attack / Release / Hold 时间。
*   **⚠️ 架构铁律**：包络跟随（Envelope Follower）与增益计算逻辑必须 **100% 在 C++ 音频线程内完成**，严禁交由 UI 侧计算或受限于 UI 刷新率。

### 2.2 DSP 音频处理多模式引擎
*   **IIR ZDF 模式 (默认)**：基于零延迟反馈（ZDF）与拓扑保持变换（TPT），消除高频频率扭曲。
*   **FIR 线性相位模式 (高级)**：支持线性相位，避免频段相位失真。
    *   **精准延迟档位**：强制量化为 Low (256 samples)、Medium (1024 samples)、Maximum (8192 samples)。
    *   **平滑切换**：切换档位时必须触发“旁路快照 (Bypass Snapshot) + 50ms 交叉淡入淡出”，绝对杜绝爆音 (Pop)。
*   **混合相位模式 (Mixed/Natural Phase)**：高阶 IIR 匹配幅频 + 全通滤波器相位补偿。
    *   **相位约束**：相位补偿量不得超过原始最小相位偏移的 **60%**，并暴露为 UI 权重滑块，防止引入可听的前回声 (Pre-ringing)。

### 2.3 通道路由与信号质量
*   **通道模式 (L/R & M/S)**：支持独立处理左/右声道或中/侧 (Mid/Side) 通道，支持通道间 Solo 与 Link 功能。
*   **过采样 (Oversampling)**：内置 2x / 4x 可选倍率。必须附带高质量的线性相位抗混叠滤波器，防止高频增益在低采样率下产生镜像混叠。

---

## 3. 灵魂交互与视觉渲染

### 3.1 真实物理响应曲线与视图交互
*   UI 渲染曲线必须基于 DSP 真实幅频响应传递函数 $\vert{}H(e^{-j\omega})\vert{}$。
*   **视图控制**：支持鼠标滚轮缩放频域（20Hz-20kHz）与拖拽平移。C++ 侧传输的点阵坐标需附带当前视图范围的裁剪逻辑。

### 3.2 高动态频谱分析仪 (Analyzer)
*   **后台平滑策略**：FFT 数据**严禁**在音频线程直接发往 UI。必须在 C++ 后台线程进行指数移动平均（EMA，时间常数约 100ms）平滑处理后，再推送至前端，确保渲染不闪烁。

### 3.3 频段独奏 (Band Solo) 与参数管理
*   **精准独奏**：单击或长按频段节点单独监听该滤波器。
*   **自动增益匹配**：必须采用 **RMS 能量匹配算法**（而非 Peak），确保听感平滑切换。
*   **Undo / Redo 栈**：由于 JUCE APVTS 原生不带 Undo 栈，需在 UI 侧维护指令快照栈 (Command Snapshot Stack)，通过 JSI 发送逆操作给 Helper 线程执行。

---

## 4. 关键架构设计与性能生命线

### 4.1 跨线程通信机制 (JSI 二进制规范)
彻底隔离音频核心与 Web UI，部署极低延迟的通信管道：
*   **高频推流 (Audio -> UI)**：采用 **三重缓冲 (Triple Buffering)** 机制推送平滑后的 FFT 数据，防止前端读取撕裂。
*   **⚠️ JSI 传输铁律**：高频参数（如拖拽时的 Gain/Freq）**严禁使用 `JSON.stringify`** 传递。必须采用二进制浮点数组（`Float32Array`）配合 `PostMessage` 的 Transferable Objects 进行零拷贝传输，将吞吐延迟压制在 100μs 以内。

### 4.2 UI 生命周期与无头扫描规避 (Headless Scan)
废弃高风险的“全局实例池化”方案，采用安全的延迟加载与彻底销毁策略：
1.  **无头规避**：当 `AudioProcessor::isNonRealtime()` 为 `true`（DAW 离线渲染或后台扫描）时，强制跳过所有 WebView 初始化，防止宿主崩溃。
2.  **异步预热**：`prepareToPlay` 阶段仅将 HTML/CSS/JS 资源异步预读至内存，**推迟 BrowserComponent 实例创建**至用户首次打开界面。
3.  **安全关闭**：UI 窗口关闭时调用 `setVisible(false)` 隐藏，并**强制销毁 JavaScript 上下文**，防止前端框架 (React/Vue) 长期在后台运行导致内存泄漏。

---

## 5. DSP 算法与实时线程硬核纪律

### 5.1 音频线程 (processBlock) 性能铁律
*   **禁止动态分配**：严禁任何 `new`, `delete`, `malloc`，警惕 `std::vector::push_back` 与动态 `std::function`。
*   **⚠️ 数学计算禁令**：**严禁在 `processBlock` 内调用 `std::sin`, `std::cos`, `std::tan` 或 `std::exp`** 进行 ZDF 系数计算。所有高频系数更新必须使用预扭曲 BLT 的 **查表法 (Look-up Table)** 或低阶多项式近似。

### 5.2 智能旁路 (Smart Bypass)
*   旁路开启/关闭时，强制执行 **50ms 的增益无损交叉淡入淡出 (Crossfade)**，杜绝 DAW 自动化写入旁路时的咔哒声。

---

## 6. 编译、分发与运行环境约束

### 6.1 包体积与引擎依赖
*   **系统原生 WebView**：严禁内嵌 CEF (Chromium Embedded Framework) 以防包体过大。强制绑定操作系统原生引擎：macOS 使用 WKWebView，Windows 使用 WebView2。
*   **体积红线**：前端静态资产需经过极度 Tree-shaking，总大小限制在 **2MB** 以内。

### 6.2 优雅降级与多平台构建
*   **Windows 兼容性**：需增加运行时检测，若 Win7/8 系统未安装 WebView2，则优雅降级至 JUCE 原生 OpenGL 图形绘制（仅显示静态曲线，确保可用性）。
*   **AAX (Pro Tools) 专供版**：鉴于 Pro Tools 对 OpenGL 上下文独占的极端敏感性，AAX 版本必须**强制剥离 WebView 依赖**，降级为纯原生 OpenGL 绘制，确保通过 Avid 官方认证审核。

---

## 7. 开发排期与高风险测试指南

*   **高风险联调期 (3-4周)**：ZDF 数学优化 + 三重缓冲 + JSI 二进制通信的联调极易引发线程死锁或 XRUN。
*   **压力测试基准**：
    1.  **极端自动化测试 (Extreme Automation)**：在 DAW 中画满方波级别的 Gain/Freq 自动化包络，测试 ZDF 拓扑是否发散。
    2.  **极限缓冲测试**：在 32 samples 极小音频缓冲区下运行，排查 UI 与 Audio 之间的锁竞争导致的掉线问题。
    3.  **动态 EQ 扫描测试**：独立环境 (Standalone) 中运行 20Hz~20kHz 正弦波慢速扫频，验证包络检测算法无低频抽吸或奇次谐波失真。