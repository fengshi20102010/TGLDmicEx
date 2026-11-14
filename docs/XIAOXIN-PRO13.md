# 联想小新 Pro 13 2020（Comet Lake, 2 麦）适配记录

> 分支：`fix/xiaoxinpro13-cml-2ch`。本文记录问题点、证据和改动，作为后续 MR 的说明材料。
> 抓包原始数据：[`captures/xiaoxinpro13-cml-2ch/`](../captures/xiaoxinpro13-cml-2ch/)（不含录音）。

## 机器信息

| 项 | 值 |
|---|---|
| 机型 | Lenovo XiaoXinPro-13IML 2020（`82DN`），BIOS `CLCN32WW` |
| CPU | i7-10710U（Comet Lake-U） |
| 音频控制器 | `8086:02C8` Comet Lake PCH-LP cAVS，子系统 `17aa:380f`，class `0x040100`（DSP 模式） |
| Codec | Realtek ALC257（AppleALC `layout-id` 11） |
| DMIC | **2 个**（NHLT：1 个 PDM 端点，2ch / 48 kHz / 16 或 32 bit） |
| macOS | 15.8.2 (24H232)，Lilu 1.7.2，AppleALC 1.9.8 |

## 现象（TGLDmic 0.6.4，`-tgldmiclilu`）

Lilu Hook 正常（`LiluHook=Yes`、`HookRouteOk=1`、`ImicStreams=1`），但每次开始录音 DSP 都启动失败，
`InjectState` 停在 1，`HookFills=0`：

```
boot: === phase 2 DSP boot attempt (Comet Lake / Cannon Lake (cAVS 1.8)) ===
boot: ADSPCS 0x001d003c PPCTL 0x00000000 before boot
...
boot: FAIL core0 power up (CPA) (0x001d003c)
inject: DSP boot failed (FAIL core0 power up (CPA) (0x001d003c))
```

冷启动、重启均无变化。

## 排除项

- **硬件 / BIOS / 固件**：Ubuntu 24.04.5 Live（内核 7.0）下 SOF 驱动正常启动 DSP（同一个 `sof-cml.ri` v2.2），
  `hw:0,6` 录音 8 秒成功，双声道约 -7 / -9 dBFS（真实语音）。见 `captures/xiaoxinpro13-cml-2ch/run.log`。
- **AppleALC / layout-id**：只配置 codec，不触碰 DSP 的 BAR4 寄存器；EFI 无 HDEF DeviceProperties，
  也没有伪装 device-id。失败点在 DSP 核心上电，和 codec 无关。

## 问题点

### 问题 1：未设置 `PPCTL.GPROCEN`，DSP 核心无法上电（已修复，待实机验证）

`PPCTL`（PP capability +0x04）的 bit 30 `GPROCEN` 是 DSP 处理功能的总开关。Linux SOF 在 probe 时通过
`hda_dsp_ctrl_ppcap_enable()` 打开它；TGLDmic 在 `Regs.hpp` 里定义了 `PPCTL_GPROCEN`，但从来没有写过。

| | PPCTL（启动前） | ADSPCS（启动前） | 结果 |
|---|---|---|---|
| 参考机（`captures/macos-v0.3.0/log.txt`） | `0x40000000`（固件已置位） | `0x00000f0f`（正常复位态） | 成功 |
| 小新 Pro 13（macOS） | `0x00000000` | `0x001d003c`（异常） | CPA 超时 |
| 小新 Pro 13（Linux） | 驱动自行置位 | 上电后 `0x0f0f0f0f` | 成功 |

参考机的固件在 macOS 启动前就把 GPROCEN 置上了，所以问题没暴露；小新的固件保持 `PPCTL = 0`。

**改动**（`DspBoot.cpp` `gating()`）：关闭时钟/电源门控时，如果 GPROCEN 是 0，就置位并记住；
在 `gating(true)` 里恢复为 0。所有启动/关闭路径（包括失败路径）都是成对调用 `gating()` 的，
因此 AppleHDA 看到的寄存器状态会被还原。新增 boot-arg `tgldmicgproc=0` 可关闭此行为。
固件已经置位时（参考机）不做任何改动，行为与 0.6.4 相同。

### 问题 2：IPC 管线写死为 4 麦（已修复，待实机验证）

`IpcReplay.hpp` 是从 HP EliteBook 840 G8（4 麦）抓的。小新的 Linux 抓包
（抓包在 `captures/xiaoxinpro13-cml-2ch/dmesg-capture-pcm6-S32_LE.txt`，由 `linux/extract-ipc.py` 生成 `kext/TGLDmic/IpcReplay2ch.hpp`，两次抓包结果完全一致）
与之相比：

| 消息 | 字段（含 8 字节头的 word 下标） | 840 G8 | 小新 Pro 13 |
|---|---|---|---|
| `DAI_CONFIG` | [21] 活动 PDM 控制器数 | 2 | **1** |
| `STREAM_PCM_PARAMS` | [18] channels（高 16 位） | 4 | **2** |
| 第一条 | `0x40020000`（`GLB_TRACE_MSG` DMA 参数） | 无 | 有，回放时应剔除 |

其余组件（`PIPELINE.10`、`EQIIR10.0` 高通、`Dmic0` 音量、`PCM6C`）一致。
Linux 加载的拓扑是 `sof-hda-generic-2ch.tplg`（原参考机为 4ch）。

**改动**：保留 4ch 表（行为不变），新增 2ch 表，运行时选择，而不是替换。

- `linux/extract-ipc.py` 重写：丢弃 `GLB_TRACE_MSG`（`0x4xxxxxxx`，配置 Linux 的 trace DMA，kext 不用）、
  支持 `--suffix/--source/--channels`，并新增 `--volume`。用同一个脚本重新生成的 4ch 表与原文件逐条一致（回归验证）。
- 新增 `IpcCommon.hpp`（公共类型与 `IpcPipeline`）、`IpcReplay2ch.hpp`（本机抓包生成）。
- **选择规则**（`Capture.cpp` `selectDmicPipeline()`，`start()` 时执行一次）：`tgldmicch=2|4` 优先；
  否则读 ACPI `NHLT`（`Nhlt.hpp`，对应 Linux `intel_nhlt_get_dmic_geo()`，1–2 麦 → 2ch，3+ → 4ch）；
  都没有则回退 4ch（保持 0.6.4 行为）。NHLT 无论有没有被 boot-arg 覆盖都会读取并写日志，不一致时会警告。
- **Dmic0 音量被固定为 0 dB**（`--volume 0x10000`）：Linux 抓包里的值是 `0xa004a`（≈ +20 dB，那次会话的 mixer 状态），
  Linux 实录里有 2–9 % 的采样接近削波（峰值 0 dBFS，中位数约 -16 dBFS）。参考机 4ch 表是 `0xe428`（≈ -1 dB）。
  按 0 dB 回放更接近参考机的电平；仍可用 `tgldmicgain=` 补偿。**这是一个取舍，尚未实测电平。**

### 问题 3：采集 / 注入路径写死 4 声道（已修复，待实机验证）

`TGLDmic.hpp` 新增 `dmicChannels`（2 或 4）、`srcFrameBytes()`、`srcRingFrames()`，下面这些全部改成按它走：

- `Capture.cpp`：`STREAM_PCM_PARAMS` 的声道数、HDA `SD_FMT`（`0x40 | (ch-1)`）、1 s 快照/包络/统计、`chPeak[]`。
- `Inject.cpp`：DSP 环形缓冲区的帧大小、插值与声道映射（`s[c % srcCh]`）、DPIB 位置换算、日志。
- `snapshot-to-wav.py`：从 ioreg 的 `DmicChannels` 读取声道数。
- ioreg 新增 `DmicChannels`、`DmicSource`、`GprocenForced`。

### 问题 4：多余核心没有断电（已修复，仅 CML，待实机验证）

Linux `cl_dsp_init` 在 ROM 应答之后、等 `INIT_DONE` 之前，会把除 `init_core_mask`（core 0）之外的 host-managed 核心
（cAVS 1.8 为 core 1–3）复位并断电（`hda_dsp_core_reset_power_down`）。我们抓到的 Linux 日志：
`reg[0x4]=0xf0f0e0e` → `0x1010e0e`，之后才是 `INIT_DONE`。TGLDmic 以 0xF 上电但从未断掉 1–3。
新增 `coreResetPowerDown()`，顺序与 Linux 相同；Tiger Lake 的掩码只有 core 0，等于不执行。失败只写日志，不中止启动。
**这一步会改变 X1C7（原作者的 CML 机器）上的行为，我无法测试，MR 里需要请 X1C7 用户验证。**

### 次要观察（暂不处理）

- PCI 0x48（`CGCTL`）：macOS 下读到 `0x007b09ff`，Linux 下是 `0x00000002`。TGLDmic 只清 bit 1（与 Linux 相同）。
  若问题 1 修复后仍有异常再排查。
- kext 启动时 `PCI CMD 0x0000` / `controller not decoded yet`：AppleHDA 尚未启用控制器，kext 会用定时器重试，目前看不影响。
- `PM: no HDA driver on this controller - no power hold`：在这台机器上没有加入 HDA 的电源树（`tgldmicpm=1` 可强制）。

## 构建说明（本地）

和 CI（`.github/workflows/build.yml`）一致：

```bash
git clone --depth 1 https://github.com/acidanthera/MacKernelSDK
git clone --depth 1 --branch 1.7.2 https://github.com/acidanthera/Lilu /tmp/Lilu-src
mkdir -p Lilu.kext/Contents/Resources
cp -RL /tmp/Lilu-src/Lilu/Headers /tmp/Lilu-src/Lilu/Library Lilu.kext/Contents/Resources/   # 注意 -L
make -C kext SDK=$PWD/MacKernelSDK
```

坑：Lilu 源码中 `Headers/hde64.h` 是符号链接，用 `cp -R` 复制会变成失效链接，
编译时报 `'Headers/hde64.h' file not found`；需要用 `cp -RL`。`hde32.h` / `hde64.h` 是相对链接（`../../hde/...`），
CI（`macos-15-intel`）里同样用的是 `cp -R`，很可能也受影响（未查看 CI 运行记录，未确认）；本分支已把 `build.yml` 改为 `cp -RL` 并加了 `test -f` 校验。

## 验证状态

| 项 | 状态 |
|---|---|
| 未修改源码基线编译 | 通过 |
| 0.6.5（问题 1–4）编译 | 通过，无新增警告；`codesign -v`、`plutil -lint` 通过 |
| NHLT 解析（主机单测，ASan/UBSan） | 通过：小新 → 2 麦，840 G8 → 4 麦；截断/损坏输入不越界（`make -C kext test`） |
| 4ch 回放表回归 | 新版 `extract-ipc.py` 重新生成的表与原文件逐条一致 |
| 2ch 回放表 | 与 Linux 抓包可重复生成；与 4ch 表逐字对比，只有预期的差异（见问题 2） |
| `IOACPIPlatformDevice` 符号 | 在 `BootKernelExtensions.kc` 中有定义；kext 声明了 `com.apple.iokit.IOACPIFamily` |
| 实机：DSP 上电 / FW_READY（0.6.5，boot-args `-tgldmiclilu tgldmicch=2`） | **通过**：`BootResult = OK FW_READY`；ADSPCS 序列 `0x0f0f0f0f → 0x0f0f0e0e → 0x01010e0e`，与 Linux 的 `0xf0f0f0f → 0xf0f0f0e → 0xf0f0e0e → 0x1010e0e` 一致 |
| 实机：管线与 Hook | **通过**：`DmicChannels=2`、`GprocenForced=Yes`、`InjectState=2`、`Streaming=Yes`，`HookFills` 持续增长（录音 8 秒后约 13 k 次 / 6.7 M 帧） |
| 实机：生命周期 | **通过**：每次录音停止后 `core powered down`、`PPCTL.GPROCEN restored (clear)`、`clock/power gating restored`，下次录音重新启动；无 panic 文件，正常关机/启动 |
| 实机：录到的声音 | **通过（初步）**：2 声道 44.1 kHz，整段 -33 dBFS RMS、峰值 -12.5 dBFS、无削波、无全零帧、安静段 -47 / 响亮段 -23 dBFS，L/R 相关但不相同。**只做了数值检查，未人工回放听感** |
| 实机：macOS 下 `getACPITableData("NHLT")` 能否取到表 | **通过**（boot-args 只有 `-tgldmiclilu`，不加 `tgldmicch`）：`NhltStatus="ok"`、`NhltBytes=6128`（与 Linux 导出的 `NHLT.bin` 长度一致）、`NhltMics=2`、`NhltArrayType=10`（0x0a）、`DmicSource="NHLT"`、`DmicChannels=2`；日志 `dmic: NHLT 6128 bytes: 2 PDM mic(s), array type 0x0a` |
| 实机：采样率 | 引擎当前是 44.1 kHz（`InjectRate=44100`），走线性重采样；在“音频 MIDI 设置”里改 48 kHz 可避免 |
| **X1C7 / 840 G8 回归** | **未验证**（问题 4 会改变 CML 上的核心断电行为） |

### 首次实机测试方案

boot-args：`-tgldmiclilu tgldmicch=2`（`tgldmicch=2` 保证走 2ch 管线，同时 NHLT 仍会被读取并记日志）。录音时：

```bash
ioreg -l -w0 -c TGLDmic | grep -E '"(DmicChannels|DmicSource|GprocenForced|InjectState|Streaming|HookFills|HookFrames|BootResult)"'
ioreg -c TGLDmic -r -l -w0 | grep -oE '"Log" = \(.*' | sed 's/","/\n/g' | grep -E 'dmic:|GPROCEN|boot: (===|cores|core|ROM|SUCCESS|FAIL)|cap:|inject:'
```

期望：`dmic: NHLT ... 2 PDM mic(s)`、`GPROCEN was clear ... set`、`cores 0xe powered down`、`SUCCESS - SOF firmware running`、
`InjectState=2`、`HookFills` 增长。出问题时按日志里最后一个 `boot:`/`cap:` 行判断卡在哪一步。

## 音质调查（0.6.5 之后；作为已知现象记录，不在本 MR 中处理）

**结论**：用户试听对比文件后，认为当前表现属于正常现象，因此底噪的根源没有继续追查，本 MR 不包含左右混音（`tgldmicmix`）的实现。
下面的数据和未确认项留给后续（如果有人想继续优化音质）。

现象：声音正常、左右都有，但底噪比耳麦大，清晰度也低一些。

### 已排除
- **重采样（44.1 kHz ↔ 48 kHz）**：安静环境下分别用原生 48 kHz 与线性重采样到 44.1 kHz 录音，12–16 kHz / 16–20 kHz 频段的噪声几乎一样（约 -71 至 -78 dBFS），差异小于环境噪声的波动（两轮之间环境噪声本身变化 2–5 dB）。折回噪声不是主因。
- 采样率每次开始录音都会回到 44.1 kHz（`InjectRate=44100`），`AudioObjectSetPropertyData(NominalSampleRate)` 设置后不会保持；系统设置的“声音”页打开时会一直占用麦克风，导致采样率无法切换。

### 主要发现：低频噪声在左右声道之间反相
20 s 录音（前约 7 s 说话，后面安静），2 ch / 44.1 kHz：

| | 噪声 | 说话 | 信噪比 |
|---|---|---|---|
| 单声道（L 或 R） | -55 dBFS | -44 至 -45 dBFS | 10.5 / 11.4 dB |
| (L+R)/2 | -66.6 dBFS | -44.6 dBFS | **22.0 dB** |
| (L+R)/2 + 120 Hz 高通 | -68.8 dBFS | -45.9 dBFS | 22.9 dB |
| 只对 L 做 80–200 Hz 高通 | 变化 < 4 dB | | 10.9–11.4 dB |

- 安静段两个声道的噪声相关系数 **-0.92**，相位差 180°（相干性 0.9），全部集中在 (L−R)/2：-51.5 dBFS；(L+R)/2 只有 -65.4 dBFS。97 % 的差分噪声能量在 500 Hz 以下，43 % 在 100 Hz 以下。
- 说话时左右相关系数 0.73–0.80（Linux 那份连续说话录音是 0.92–0.94）。
- 噪声不是直流偏置（直流 < -120 dBFS），也不是固定频率的市电谐波（频谱是 40–500 Hz 的宽带）。
- 安静段噪声随时间缓慢上升（-54 → -51 dBFS，约 12 s），说话时 (L−R)/2 的 200 Hz 以下能量反而更低（-58.9 vs -52.7 dBFS）。原因不明。
- 真正的环境声学噪声在相邻两个麦克风上应该同相。反相说明它更可能是电气/DSP 层面的东西，**但这只是推断，没有证据确认来源**。

### 未确认
1. 是硬件/DSP 固有特性，还是我们的流程（DAI_CONFIG 回放、PDM 时钟边沿/极性、`Dmic0` 音量）与 Linux 不同？Linux 抓包里没有真正安静的片段，无法比较。
2. 是不是 Hook 引入的？Hook 对两个声道做完全相同的处理，按代码推理不会产生反相噪声，但没有实测。可用 `-tgldmiccap`（直接从 DSP 环形缓冲区取一秒）绕过 Hook 验证。

### 可行的缓解（尚未实现）
把 (L+R)/2 同时输出到两个声道。两个相邻麦克风的立体声差异本来就没有实际意义（语音通话/录音以单声道为主），而本机上这样做的信噪比提升约 11.5 dB。建议做成 boot-arg（如 `tgldmicmix=mono`），默认保持立体声，不改变其他机型的行为。

### 试听文件
`~/Downloads/tgldmic-listen/`：`v3_raw_stereo_as_recorded.wav`（原样）、`v0_original_L.wav`（仅左声道）、`v1_mono.wav`（(L+R)/2）、`v2_mono_hp120.wav`（(L+R)/2 + 120 Hz 高通）。后三个已按峰值归一化到 0.9，所以响度不能直接比较，只比较底噪和清晰度。

## 待办（TODO）

按优先级：

1. **核心断电（问题 4）加开关并验证是否必须。** 目前 Comet Lake 机器默认都会执行 `coreResetPowerDown()`，没有 boot-arg 可以关闭，也没有单独验证过“不加这一步是否也能启动”。计划：新增 `tgldmiccorepd=0` 开关；在小新上关闭后测试 DSP 启动与录音。关闭也正常 → 默认改为关闭（其他机器行为不变）；关闭不行 → 再设计判断条件。
2. **回归验证 X1C7（Comet Lake，4 麦）和 EliteBook 840 G8（Tiger Lake，4 麦）。** 本 MR 只在小新 Pro 13（82DN，macOS 15.8.2）上做过实机验证，其他机型是按代码推理，没有实测。需要这两类机器的用户帮忙确认：DSP 能启动、`DmicChannels=4`、`DmicSource="NHLT"`、录音正常。
3. **版本号提交单独处理。** 0.6.4 → 0.6.5 单独放在最后一个提交，维护者可以直接丢弃或改成自己的版本号。
4. **新增的 `com.apple.iokit.IOACPIFamily` 依赖只在 macOS 15.8.2 上验证过。** 在 Ventura / Sonoma / Sequoia 其他小版本上需要确认能正常加载（解析不了依赖会导致整个 kext 不加载）。
5. **CI：确认 `cp -RL` 的改动在 GitHub Actions（`macos-15-intel`）上通过。** 本地验证过，没有看过 CI 运行结果。
6. **（可选）音质：左右混音。** 见“音质调查”。安静段噪声在左右声道间反相，(L+R)/2 可把信噪比从约 11 dB 提到约 22 dB。做成 `tgldmicmix=mono` 开关，默认保持立体声。需要先确认噪声来源（硬件固有、DSP 配置，还是 Hook），可用 `-tgldmiccap` 绕过 Hook 取一秒 DSP 数据对比，或在 Linux 下录一段真正安静的音频。
7. **（可选）采样率。** 每次开始录音引擎都会回到 44.1 kHz，经线性重采样；数据显示这不是底噪的主因，但原生 48 kHz 仍是最直接的路径。可以研究让 Hook 在引擎设置 48 kHz 时跳过重采样的路径是否已经覆盖（`injectStep == 1.0`）。
8. **DSP 电源管理优雅化。** 沿用上游 README 的“后期优化”：目前靠 100 ms 定时器轮询判断，后续可考虑 Hook AppleHDA 的麦克风启停 API。
