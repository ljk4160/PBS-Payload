<div align="center">

<img src="payload/web/logo.png#gh-dark-mode-only" alt="PBS" width="180" />
<img src="payload/web/logo-dark.png#gh-light-mode-only" alt="PBS" width="180" />

<br/><br/>

**PlayStation 5 广播流采集 payload 与浏览器查看器 —— 同一代码树内附带 PS4 后端。**

**中文翻译版**：由 [ljk4160](https://github.com/ljk4160) 维护，在原版基础上包含多项固件 12.00 修复与功能增强（见 [CHANGELOG.md](CHANGELOG.md) 中文更新记录）。

<br/>

[![C11](https://img.shields.io/badge/C-11-informational?style=flat-square&logo=c&logoColor=white)](https://en.cppreference.com/w/c/11)
[![License](https://img.shields.io/badge/license-GPL--3.0--or--later-informational?style=flat-square)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-PS5%20%7C%20PS4-blueviolet?style=flat-square)](#-支持的固件)
[![Version](https://img.shields.io/badge/version-0.4.0-blue?style=flat-square)](VERSION)
[![Firmware](https://img.shields.io/badge/PS5%20profiles-21%20measured-brightgreen?style=flat-square)](docs/PS5_FIRMWARE_OFFSETS.txt)

<br/>

[概述](#概述) · [功能](#功能) · [支持的固件](#支持的固件) · [使用](#使用) · [构建](#构建) · [HTTP API](#http-api) · [已知限制](#已知限制) · [文档](#文档)

<br/>

</div>

---

## 仓库架构

主机端程序按职责拆分在 `payload/src/common/` 与 `payload/src/platform/{ps5,ps4}/` 下，
各固件的偏移表保存在选择它们的代码旁边（`platform/*/offset/`）。测量工具
（dump payload、生成器、账本）位于 `tools/`，刻意置于采集路径之外。生成的资源
保存在 `payload/build/generated/`。

浏览器查看器在构建期嵌入 payload，因此单个 ELF 同时承载采集、HTTP 传输与
UI。依赖规则见 [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)，发布历史见
[`CHANGELOG.md`](CHANGELOG.md)。

## 概述

PBS Payload 运行在主机上，采集实时视频流并通过局域网提供给浏览器：MJPEG 帧、
RTP 封装的 H.264，或写入主机本地的录制文件。同一个查看器即可启动、停止和
切换采集配置，无需重新加载 payload。

payload 对固件内存的每一次操作都来源于**该固件本身**的内存转储，绝不借用
相邻版本的。一个 profile 就是一张指令地址表——Avcap2 请求几何与 RemotePlay
构造器——错误的地址不是可容忍的软失败，因此未知固件会被**设计上直接拒绝**。

```
设计规则
  ├── 实测高于猜测      → profile 只有在 dump 并反汇编之后才存在
  ├── 失败即关闭        → 未知请求尺寸 = 拒绝，绝不借用
  ├── 只说验证过的话    → 文档只写硬件上验证过的内容
  ├── 小作用面          → 一个 HTTP 服务，一个内嵌查看器，无外部运行时
  └── 附加项皆需主动开启 → 品牌横幅、dump 与诊断都是构建开关
```

## 功能

| 领域 | 提供的能力 |
|---|---|
| **采集** | 通过 Avcap2 的硬件 H.264 编码器；720p / 1080p / 1440p / 2160p 配置，60 fps 模式 |
| **配置切换** | 运行时通过 `/capture/profile` 切换；PS5 在检测到 1080p 输出模式时自动选择 1080p |
| **音频** | 立体声 48 kHz float32 PCM（`/audio`） |
| **传输** | HTTP MJPEG（`/frames`）、外部播放器用的 RTP 封装、采集重启与断流处理 |
| **查看器** | 内置浏览器 UI：实时统计、分辨率切换、强制关键帧、录制、下载 |
| **录制** | 在主机上写入 H.264 文件，从 `/recordings/` 取回 |
| **固件** | 24 个实测 PS5 profile 及经审计的家族范围；PS4 运行时布局发现 |
| **诊断** | 偏移、依赖与模块 dump payload，用于产出下一个固件的 profile |
| **可靠性** | 端口回退（十个候选）、RTP 积压重同步伴随关键帧请求、周期性关键帧刷新 |
| **中文界面** | 内置查看器支持中英双语切换，选择持久保存 |
| **码率上限** | Web 界面实时设置编码码率上限（1–100 Mbps），热生效并自动强制关键帧 |
| **驻留恢复** | 停止直播后进程驻留待命，Web 一键恢复，无需重新注入 |

默认编码器预算为 60 Mbit/s，可配置范围 1–100 Mbit/s。实际输出取决于场景与
编码器——更高的预算并不保证更高的实测码率。

## 支持的固件

### PS5 —— 实测 profile

每一行都来自该固件自身的 dump 反汇编：请求几何来自
`libSceAvcap2.sprx.prefix.bin`，RemotePlay 构造器来自 `SceRemotePlay` 的
EBOOT。固件 ID 格式为 `0xMMMMmmmm`（主版本、次版本、构建号）。

| 版本 | 固件 ID | 请求尺寸 | `kind` |
| --- | --- | --- | --- |
| 3.20 | `0x03200004` | `0x218` | `0xe8` |
| 4.00 | `0x04000042` | `0x230` | `0x100` |
| 4.51 | `0x04510001` | `0x230` | `0x100` |
| 5.00 | `0x05000040` | `0x298` | `0x168` |
| 5.10 | `0x05100023` | `0x298` | `0x168` |
| 5.50 | `0x05500008` | `0x298` | `0x168` |
| 6.02 | `0x06020004` | `0x2a8` | `0x168` |
| 7.20 | `0x07200005` | `0x2a8` | `0x168` |
| 8.40 | `0x08400005` | `0x2a8` | `0x168` |
| 9.20 | `0x09200005` | `0x2b0` | `0x168` |
| 9.40 | `0x09400008` | `0x2b0` | `0x168` |
| 9.60 | `0x09600004` | `0x2b0` | `0x168` |
| 10.01 | `0x10010000` | `0x2b8` | `0x188` |
| 10.60 | `0x10600006` | `0x2b8` | `0x188` |
| 11.20 | `0x11200005` | `0x2e0` | `0x1a0` |
| 11.40 | `0x11400005` | `0x2e0` | `0x1a0` |
| 11.60 | `0x11600005` | `0x2e0` | `0x1a0` |
| 12.00 | `0x12000043` | `0x2e0` | `0x1a0` |
| 12.40 | `0x12400009` | `0x2e0` | `0x1a0` |
| 12.70 | `0x12700001` | `0x2e0` | `0x1a0` |
| 13.00 | `0x13000040` | `0x2e0` | `0x1a0` |
| 13.20 | `0x13200006` | `0x2e0` | `0x1a0` |
| 13.42 | `0x13420000` | `0x2e8` | `0x1a8` |
| 13.60 | `0x13600007` | `0x2e8` | `0x1a8` |

其中十个为**仅精确匹配**（3.20、4.00、4.51、5.00、5.10、5.50、9.60、11.20、13.00、13.20）：
同一版本的另一构建号会被拒绝，而不会用从未实测过的几何参数去服务它。

### PS5 —— 由经审计的家族范围覆盖的版本

在实测锚点之间，Avcap2 几何未发生变化的版本无需自己的 profile 即可接受：

| 家族范围 | 锚点 profile | 覆盖的版本 |
| --- | --- | --- |
| 6.02 – 8.40 | `602` | 6.50, 7.00, 7.01, 7.40, 7.60, 7.61, 8.00, 8.20 |
| 9.20 – 9.40 | `920` | — |
| 10.01 – 10.60 | `1001` | 10.20, 10.40 |
| 11.40 – 12.70 | `1160` | 12.02, 12.20, 12.60 |
| 13.40 – 13.60 | `1360` | 13.40 |

家族范围只覆盖审计窗口内的版本，绝不覆盖列为仅精确匹配的版本，也绝不覆盖
仅精确匹配版本的另一构建号。

### PS5 —— 尚不支持

| 版本 | 拒绝原因 | 需要什么 |
| --- | --- | --- |
| 9.00 | 在 9.20–9.40 家族范围之外 | `libSceAvcap2` + `SceRemotePlay` 的 dump |
| 11.00 | 介于 10.01–10.60 几何（`0x2b8`）与实测的 11.20 构建（`0x2e0`）之间 | dump |
| 14.00 | 超出最后一个审计家族（13.40–13.60） | dump |
| 14.10 | 超出最后一个审计家族（13.40–13.60） | dump |

如果你的主机是上述版本之一，payload 会拒绝打补丁而不是冒错误的请求尺寸的
风险。请运行 dump 构建并携带归档开一个 issue——3.20、4.00、4.51、5.10、11.20、
13.00、13.20、13.42 等其他 profile 正是这样加入的。

### PS4

| 版本 | 状态 |
| --- | --- |
| 14.00 | 硬件验证（fw `0x14008001`）：内核布局运行时发现（acmgr `+0x3b3830`，gates `+0x3673f5`/`+0x3678e1`），gates 已布防，采用 ptrace 身份，Avcap Init/Open/Start 全部 `0x0`，720p worker 后端通过 HTTP 串流 |
| 11.00 | 硬件验证：720p worker 后端，180 秒会话约 59.9 fps，通过导出的 `sceVshAvcapReadVideo` 读取视频 |
| 9.00 | 内核布局（acmgr 与两个 gates）运行时发现；Avcap 导出已测量；采集内部未验证 |
| 6.72 | 仅偏移报告与诊断 |

PS4 内核偏移在不同版本之间不通用，因此没有匹配布局的构建会在打任何补丁
之前被拒绝。PS4 音频已禁用。更多细节：
[`docs/PS4_FIRMWARE_OFFSETS.txt`](docs/PS4_FIRMWARE_OFFSETS.txt)、
[`docs/PS4_HANDOFF.md`](docs/PS4_HANDOFF.md)。

## 使用

用你的 payload 加载器加载 `pbs.elf`。不带参数时，较新的 profile 会在 9999
端口以 2560×1440 启动。实测的 3.20、4.00、4.51 与 5.10 构建以 1920×1080 启动，
并提供 720p、1080p 与 2160p：它们的原生分辨率表没有 1440p 项。

```
pbs.elf avserve [720p|1080p|1440p|2160p] [bitrate=N] [minqp=N] [norate]
                [offsetdump] [dependencydump] [moduledump] [noaudio] [nocontroller] [port]
```

在同一网络的浏览器中打开 `http://<主机IP>:9999/`。音频在用户交互后开始
（浏览器自动播放策略）。如果端口已被其他进程占用，payload 会在十个端口的
窗口内绑定下一个空闲端口，并通过主机通知告知实际端口。

payload 作为常驻服务在主机上运行；查看器关闭时它继续采集。`POST /stop`
释放采集资源。**本翻译版增强**：停止后进程驻留待命，Web 界面点击"恢复直播"
即可就地重开，无需重新注入。

## 构建

需要 LLVM（clang + lld）与 payload SDK：

| 目标 | SDK | 输出 |
| --- | --- | --- |
| PS5 | [`ps5-payload-dev/sdk`](https://github.com/ps5-payload-dev/sdk) | `payload/build/ps5/pbs.elf` |
| PS4 | [`ps4-payload-dev/sdk`](https://github.com/ps4-payload-dev/sdk) | `payload/build/ps4/pbs-remote-stream.elf` |
| PS5 dump | 同 PS5 SDK | `payload/build/pbs-ps5-dump.elf` |

```bash
# Linux / WSL
export PS5_PAYLOAD_SDK=$HOME/ps5-payload-sdk
./payload/build.sh ps5
```

```powershell
# Windows（WSL 后端）
./payload/build.ps1 -Platform PS5 -SdkPath '~/ps5-payload-sdk'
```

可选开关：`-WebLogo`（WSL 中为 `PBS_WEB_LOGO=1`）嵌入所有者横幅，`-Kind dump`
构建测量 payload，`-Ps5DefaultResolution` 修改初始配置。离线回归测试用
`tools/test_ps4_host.sh`，CI 走 `verify.yml`。完整说明：
[`BUILDING.md`](BUILDING.md)，发布流程：[`docs/RELEASING.md`](docs/RELEASING.md)。

本翻译版另附 **Windows 原生构建路径**（无需 WSL）：LLVM 便携版 +
ps5-payload-sdk 官方预编译包（`win/prospero-clang.cmd`），详见 CHANGELOG。

## HTTP API

| 端点 | 方法 | 用途 |
| --- | --- | --- |
| `/` | GET | 内嵌查看器 |
| `/config.json` | GET | 当前配置、可用分辨率、解码器头 |
| `/status.json` | GET | 宽、高、fps、码率、录制状态、驻留状态 |
| `/frames` | GET | MJPEG 流 |
| `/audio` | GET | 原始 PCM（立体声，48 kHz，float32） |
| `/capture/profile` | POST | 请求分辨率切换（`?height=1080`） |
| `/capture/bitrate` | GET/POST | 查询/设置码率上限（`?kbps=20000`，热生效） |
| `/capture/start` | POST | 恢复驻留中的采集（配合 `/stop`） |
| `/capture/restart` | POST | 重启采集管线 |
| `/video/keyframe` | POST | 为当前查看器强制 IDR |
| `/record/start`, `/record/stop` | POST | 在主机上写入或停止录制 |
| `/recordings`, `/recordings/<name>` | GET | 列出与下载录制 |
| `/stop` | POST | 停止直播并驻留待命（进程保持存活） |
| `/banner.png` | GET | 所有者横幅，仅在 `PBS_WEB_LOGO=1` 构建中存在 |

这些端点是**无身份验证**的本地网络服务。请使用可信局域网，切勿将主机暴露
到互联网。

## PS5 远程手柄

内嵌查看器为 payload 已支持的 PS5 固件 profile 提供**连接手柄**功能。显式
连接后，如果主机询问请选择一个 PS5 用户。点击手柄区域使用其显示的键盘绑定，
使用标准浏览器手柄，或在视频上按住鼠标右键移动右摇杆。同一时间只有一个
查看器拥有手柄。离开会话前请用**断开手柄**释放。

串流与手柄输入默认包含在同一个 `pbs.elf` 中。启动时传入 `nocontroller`
（`pbs.elf avserve nocontroller`）可为该次会话禁用输入。构建期排除原生手柄
后端：`payload/build.ps1` 用 `-Ps5Controller 0`，`payload/build.sh ps5` 用
`PBS_PS5_CONTROLLER=0`。两个构建脚本的默认值都是 1。禁用输入后，查看器
隐藏手柄区域且无法注册。

250 ms 无更新即释放输入；两秒后查看器会话被释放，但虚拟设备在 payload
生命周期内保持注册。重连会复用该设备而不是创建新的。失去焦点发送中性输入。
视频配置切换保持手柄会话。只有 payload 停止时才移除设备与进程特权。基本
按键、摇杆与扳机已实现；触摸坐标、体感与震动反馈未实现。此功能仅限 PS5。

后端在本地解析原生 `libScePad` 导出并使用现有固件选择器。本翻译版在固件
12.00 上的修复：改用**链接期导入**取代对新鲜 LoadStartModule 句柄的运行时
dlsym——后者在 12.00 上会导致 payload 在首次 `scePadInit` 之前崩溃。离线
测试覆盖全部 24 个实测 profile ID、封帧、会话所有权、浏览器焦点丢失与清理
失败。每个固件的硬件验证仍在进行中。连接时检查 API 可用性；失败即停止操作。
诊断日志位于 `/data/pbs/controller.log`。

对查看器集成方，`GET /input/status` 报告可用性与占用而不暴露会话 token。
`POST /input/connect` 返回 token；在 `/input/state` 与 `/input/disconnect`
中用 `X-PBS-Controller` 头携带。三条 POST 路由都要求 `X-PBS-Input: 1`。
状态体包含八个无符号十进制 CSV 字段：`sequence,buttons,lx,ly,rx,ry,l2,r2`。
摇杆中心值 128，轴与扳机范围 0–255，序列号须按 2^32 取模递增。连接期间
至少每 100 ms 发送一次状态。删除只在 payload 关闭时发生；若失败，
`cleanup_required` 与 `last_rc` 会报告在状态与日志中。

## 已知限制

- **硬件验证按设计是不均匀的。** PS4 11.00 的 720p 已在真实硬件上验证
  （180 秒会话，约 59.9 fps，解码录制）。PS5 profile 通过离线反汇编与边界
  检查验证；每次发布的主机冒烟测试是最后一步，部分 profile 尚未完成。
- **不承诺原生 4K、120 fps 或零延迟。** 2160p 是可选的采集配置，不是对
  编码器或链路的承诺。
- **PS4 音频已禁用。** PS4 采集在 11.00 与 14.00 的 720p 上已验证；其余
  PS4 固件未验证。
- **上表之外的固件会被拒绝**，这是有意为之。
- 桌面/移动应用是独立项目，不属于本仓库或其许可授予范围。

## 文档

| 文档 | 内容 |
| --- | --- |
| [`BUILDING.md`](BUILDING.md) | 工具链、SDK、全部构建开关 |
| [`CHANGELOG.md`](CHANGELOG.md) | 发布历史（含中文更新记录） |
| [`docs/PS5_FIRMWARE_OFFSETS.txt`](docs/PS5_FIRMWARE_OFFSETS.txt) | 各版本几何、来源 dump 与证据 |
| [`docs/PS5_FIRMWARE_RANGES.md`](docs/PS5_FIRMWARE_RANGES.md) | 家族范围覆盖的原理与边界 |
| [`docs/PS5_OFFSET_PROFILES.md`](docs/PS5_OFFSET_PROFILES.md) | dump 如何变成 profile |
| [`docs/STREAM_PROFILES.md`](docs/STREAM_PROFILES.md) | 运行时分辨率切换 |
| [`docs/PS4_HANDOFF.md`](docs/PS4_HANDOFF.md) | PS4 硬件证据与待办 |
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | 模块边界与依赖规则 |
| [`docs/RELEASING.md`](docs/RELEASING.md) | 发布流程、商店注册、SDK 供给 |

## 支持开发

如果 PBS Payload 对你有用，并希望支持持续开发：

[![Support me on Ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/seregon)

## 致谢

- **John Törnblom** — PS5 payload SDK 与本实现参考的 `ptrace` 进程调用技术
  （prospero-menu/libNineS）。
- **ps4-payload-dev** — PS4 payload SDK 与参考工具。
- **MemDBG** — 进程内存访问。
- **PlayStation 自制软件社区** — 测试者、dump 贡献者与反馈者；本仓库每个新
  固件 profile 都来自社区 dump。
- **ljk4160** — 固件 12.00 问题诊断与修复（手柄崩溃、帧缓冲 fallback、
  驻留恢复）、码率上限与中文界面。

## 许可与来源

包括内嵌查看器在内的源代码以 **GPL-3.0-or-later** 授权——见
[LICENSE](LICENSE)。既有署名予以保留。

`payload/web/logo.png`（亮色，深色背景用）与 `payload/web/logo-dark.png`
（同一标志，浅色背景用的反色版）是 PBS 品牌资产，不是 GPL 授权的美术作品：
它们仅为配合本项目使用而包含，无关的复用需要所有者许可。README 通过
GitHub 的 `#gh-dark-mode-only` / `#gh-light-mode-only` 片段选择正确的版本。

`payload/web/banner.png` 是在 `/banner.png` 服务的串流叠加图：仅在构建要求
时（`PBS_WEB_LOGO`）嵌入，同样不是 GPL 授权，默认构建不包含它。

<div align="center">

以 [GNU General Public License v3.0 or later](LICENSE) 发布

</div>
