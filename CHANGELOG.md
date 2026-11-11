# Changelog

Notable changes to PBS Payload. `VERSION` at the repository root holds the
numeric version; a release tag may add a suffix (`v0.2.0-beta.1`).

# 更新日志（中文）

PBS Payload 的重要变更。仓库根目录的 `VERSION` 保存数字版本号；发布标签可以
带后缀（如 `v0.2.0-beta.1`）。

## v0.4.0-beta5-fix0.4（本 fork 的增强版）— 2026-11-11

在原版 v0.4.0（含上游 PR #3 修复）基础上，针对固件 12.00（`0x12000043`）的
实测问题做的全套修复与功能增强。全部改动已在 PS5 实机（12.00）上硬件验证。

### 固件 12.00 修复

- **修复 Web 界面点击手柄导致 payload 崩溃**。12.00 上 `libScePad.sprx` 无法
  按名解析，每次连接都走 `LoadStartModule` 获取新句柄，而对新鲜句柄做
  `kernel_dynlib_dlsym` 会在首次 `scePadInit` 日志之前触发崩溃（每次点击
  Connect 都复现）。修复：`prepare()` 改用**链接期导入**（SDK stub
  `libScePad`），跳过运行时 dlsym；按名解析成功的固件仍走原路径，无回归。
  验证：完整的虚拟手柄生命周期（scePadInit → SetProcessPrivilege →
  VirtualDeviceAddDevice → InsertData ×N → DeleteDevice → 特权回收）。
- **根治"反复注入后永远启动不了"**。诊断发现 SceRemotePlay 用户堆在长期
  串流与多次采集重启后碎片化，远程 `malloc(0x200000)` 帧缓冲开始返回 0，
  此后每次注入都在 `GetNonVclNalUnitStream` 之前静默失败。修复：worker 引导
  时一次性 `mmap` 保留 2MB 私有帧缓冲（`PBS_PS5_WORKER_FRAME_SIZE`），采集
  重启/分辨率切换/断流重连**不再向 SceRemotePlay 堆分配内存**；目标 malloc
  失败时自动回退到该保留缓冲（inject.txt 可见 `using worker frame buffer`）。
  worker 退出时随 mapping 一并释放，无泄漏。
- **替换旧实例的竞态修复**。注入时自动 SIGKILL 同名旧实例（原版没有自保
  逻辑，klogsrv/elfldr 都有）后，原实现盲等固定 2 秒，偶尔输给内核的
  TIME_WAIT 回收，导致新实例 bind 9999 失败并静默退出。修复：改为**探测式
  等待**——每 200ms 实际尝试绑定 9999 直到成功（上限 10 秒）。

### 新功能

- **Web 码率上限**：新端点 `POST/GET /capture/bitrate?kbps=N`（合法范围
  1000–100000，0 = 恢复启动码率）。查看器 dock 栏新增下拉（默认 60M /
  20M / 10M / 5M / 2M），设置热生效并自动强制一次关键帧让新码率立即起作用；
  设置在断流重连与采集重启之后保持。外网带宽不足导致的反复断流以此对症。
- **停止后驻留 + 一键恢复**：`/stop` 之后 payload 不再退出——HTTP 监听器
  保持存活，`status.json` 报告 `"parked":true`，屏幕弹出 "PBS parked"，
  查看器显示驻留提示与**恢复直播**按钮；`POST /capture/start` 唤醒采集并
  就地重开（复用同一 worker 与帧缓冲，不重新注入、不重新引导 elfldr、
  不重启主机）。恢复路径即原 `goto avopen_retry`，帧缓冲已预留故不会
  再次触碰目标堆。
- **查看器中英双语界面**：全部 UI 文案（标题、状态、录制、存储、手柄、
  分辨率、对话框）提供中文翻译，dock 栏语言下拉即时切换，选择存于
  localStorage 刷新后保持。手柄状态提示（占用/过期/清理失败）一并翻译。
- **注入替换旧实例**：新实例启动时查找并 SIGKILL 所有同名旧 payload 进程
  （跳过自身），inject.txt 记录 `PBS replace: killed N previous instance(s)`，
  屏幕通知 "PBS: replaced previous instance"。反复注入从"双实例打架"变成
  "干净替换"。

### 诊断增强

- `capture_stream` 的三个静默 `return -1`（导出解析失败 / 帧缓冲分配失败 /
  seconds 非法）全部补上明确日志——此前失败只能看到 OpenVideo ok 之后
  戛然而止。
- HTTP server bind 失败（端口被占用且扫描窗口耗尽）现在会写日志并弹屏通知，
  不再静默退出。
- `status.json` 新增 `"parked"` 字段。

### 硬件验证（PS5 retail，固件 0x12000043）

| 构建 | 请求路径 | OpenVideo | 手柄 | 串流 |
|---|---|---|---|---|
| v0.4.0-beta.3 | synthetic | ok | ok | ✅ |
| v0.4.0-beta.4 / beta.5 | firmware-constructor（未修） | `0x81950001` | 崩溃 | ❌ |
| master `a710f55` + 本 fork 修复 | firmware-constructor（修后） | ok | ok | ✅ |
| 长稳测试 | 同上 | — | — | 1440p 868s @ 59.9fps，3 次断流自动重连 |

相关上游 issue：[#9](https://github.com/seregonwar/PBS-Payload/issues/9)
（12.00 firmware-constructor 回归），修复源自
[#3](https://github.com/seregonwar/PBS-Payload/pull/3)（13.00 同型问题）。

### Windows 原生构建记录

不依赖 WSL 的构建路径已验证可行：LLVM 便携版（clang 23.1.5）+ ps5-payload-sdk
官方预编译包（v0.45，`win/prospero-clang.cmd`）。要点：SDK 的
`prospero-llvm-config` 需要 `llvm-config-23` 在 PATH 中（复制 llvm-config.exe
并重命名）；lld 按 argv0 分发驱动，`bin/prospero-lld` 需指向内置 ELF driver
的 `win/lld.exe`；产物通过 `tools/verify_ps5_elf.py` 校验。

## v0.4.0-beta.2 — 2026-11-07

Beta, pre-release. Changes since `v0.4.0-beta.1`: PS5 capture now runs through
a detached worker and does not retry a rejected `OpenVideo` in the same payload
execution; the virtual controller survives viewer reconnects until shutdown.

### PS5 capture

- Boot a private RPC worker inside `SceRemotePlay`, then detach ptrace. The
  bootstrap resolves its exports by name, allocates its own RW mapping and
  switches it to RX; it uses no firmware offsets and does not overwrite
  executable padding. Sustained capture is refused if the worker cannot start.
  Shutdown sends the worker exit command, joins it and releases the mapping.
- Quarantine a failed `OpenVideo`. A successful `STOP`/`ClearCallback`/
  `Terminate` sequence proves only that PBS released its public objects, not
  that the backend freed the encoder slot, so a retry could keep returning
  `0x80c78000`. The payload now reopens capture only after it owned and closed
  a real video handle in the same execution.
- Remove the automatic 1080p retry after a `0x81950008` rejection. For the
  automatic 1440p profile on 10.01 (`0x10010000`), start at 1080p before the
  first open instead; explicit 1440p and 4K requests are unchanged.
- Find and validate the native `SceRemotePlay` request in target memory for the
  requested geometry, feature, usage, resolution and frame-rate mode. This
  release logs the result in `inject.txt` with reuse disabled: the payload
  still builds its synthetic request.
- Record the deterministic constructor writes for all 24 exact PS5 profiles as
  per-field initializers, with the generator refusing dynamic or conflicting
  stores. Exact profiles now apply these firmware-native constants before
  overwriting caller-owned resolution, feature, frame-rate, rate and video-mode
  fields. Family fallbacks remain explicitly synthetic in the runtime log.
- Treat ResourceArbitrator `0x819b0007` as an already-registered callback:
  continue with a borrowed registration and, if PBS's RUNNING transition
  succeeds, balance it with STOP without clearing another owner's callback.

### Controller

- Keep the virtual PS5 controller device for the payload lifetime. A viewer
  disconnect only ends the session; `connect` reuses the existing device and
  `pbs_input_shutdown()` performs the final delete after the input thread has
  stopped. This removes the repeated `AddDevice`/`DeleteDevice` churn that
  exhausted the `libScePad` device pool across reconnects. Process privilege is
  retained until the same shutdown.
- Use the positive module handle returned by `sceKernelLoadStartModule()`
  directly. This fixes firmwares such as 10.60 where `libScePad` loads as
  `0x2023` but an immediate lookup by basename still fails.
- Quarantine a virtual device after `InsertData` fails. Reconnect is disabled
  for that payload execution, while final shutdown can still delete the known
  device ID without blind retries or pool churn.

### Worker and tooling

- Raise the worker RPC copy limit to 2 MiB and use inline byte-copy helpers in
  the relocated remote blob, so large frame reads do not depend on compiler
  helpers outside the mapping.
- Add the PS5 worker, request-template matcher and shared PS4 worker sources to
  both build scripts.
- Extend the offline suite with PS5 worker boot/shutdown, 24-profile request
  signature and initializer checks, capture quarantine tests and persistent
  controller lifecycle tests.
- Preserve and quarantine worker state after ambiguous `pthread_create`,
  unreadable thread handles, uncertain RPC exit or failed cleanup. A second
  bootstrap cannot overwrite a possibly live mapping, and late boot failures
  no longer cause a second detach in the caller.
- When capture uses the in-target worker, dependency diagnostics record export
  RVAs without reading module text. This avoids faulting the worker on
  execute-only mappings while preserving the RPC channel required for cleanup.

### Known limits

- The PS5 worker bootstrap and native request matcher are covered by host-side
  tests; a console smoke test remains the final step before publishing the build.

## v0.2.0-beta.1 — 2026-10-29

Beta, pre-release. First release of the 0.2.0 line: both console payloads build
from this tree and the PS5 side ships 20 measured firmware profiles.

### Assets

| Asset | Platform | Loaded on |
| --- | --- | --- |
| `PBS-Payload-0.2.0-beta.1-ps5.elf` | PS5 | PS5 |
| `PBS-Payload-0.2.0-beta.1-ps4.elf` | PS4 | PS4 11.00 worker backend |

Each asset ships with a `.sha256` sidecar and `SHA256SUMS.txt` covers the whole
version.

### PS5 firmware coverage

Every profile is disassembled from a memory dump of the running console: the
Avcap2 request geometry comes from `libSceAvcap2.sprx.prefix.bin`, the
RemotePlay constructor from the `SceRemotePlay` EBOOT. Nothing is guessed from a
neighbouring version.

| Release | Firmware | Request | `kind` |
| --- | --- | --- | --- |
| 3.20 | `0x03200004` | `0x218` | `0xe8` |
| 4.00 | `0x04000042` | `0x230` | `0x100` |
| 4.51 | `0x04510001` | `0x230` | `0x100` |
| 6.02 | `0x06020004` | `0x2a8` | `0x168` |
| 7.20 | `0x07200005` | `0x2a8` | `0x168` |
| 8.40 | `0x08400005` | `0x2a8` | `0x168` |
| 9.20 | `0x09200005` | `0x2b0` | `0x168` |
| 9.40 | `0x09400008` | `0x2b0` | `0x168` |
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

Between the measured anchors, releases whose Avcap2 geometry is unchanged are
served by an audited family range instead of being refused:

| Family | Anchor | Releases accepted without their own profile |
| --- | --- | --- |
| 6.02–8.40 | `602` | 6.50, 7.00, 7.01, 7.40, 7.60, 7.61, 8.00, 8.20 |
| 9.20–9.40 | `920` | — |
| 10.01–10.60 | `1001` | 10.20, 10.40 |
| 11.40–12.70 | `1160` | 12.02, 12.20, 12.60 |
| 13.40–13.60 | `1360` | 13.40 |

Deliberately refused until a dump exists: **9.00**, **9.60**, **11.00**,
**14.00** and **14.10**. A wrong request size is not a soft failure, so an
unknown geometry never borrows another release's. 3.20, 4.00, 11.20, 13.00 and
13.20 are also exact-only: another build of the same release is refused rather
than served with a geometry it was never measured on.

### PS5 dump and ledger tooling

- `payload/build.ps1 -Platform PS5 -Kind dump` builds the dump payload
  (`build/pbs-ps5-dump.elf`) that saves the module images a profile is derived
  from, including `libSceAvcap2.sprx.prefix.bin` and the `SceRemotePlay` EBOOT;
  `payload/build.sh ps4 dump` produces the PS4 offset report the PS4 ledger is
  filled from.
- `tools/generate_ps5_offsets.py` turns a dump archive into a profile, reports
  archives whose name disagrees with the firmware they contain, and refuses
  ambiguous or conflicting images instead of picking one.
- `tools/data/ps5_firmware_offsets.json` records the evidence behind every
  profile; `tools/ps5_firmware_table.py --check` fails when the ledger, the
  profiles, `registry.c`, the family ranges and the public release catalog drift
  apart. `--list`, `--unknown`, `--compare` and `--write` inspect and regenerate
  the tables in `docs/PS5_FIRMWARE_OFFSETS.txt`.

### PS4

- Remote AVCap worker backend: reads video through exported
  `sceVshAvcapReadVideo`, no private ring offsets.
- Dynamic layout: kernel gate addresses are discovered from executable ELF
  segments and checked instruction context, the trampoline from live import
  padding; missing or ambiguous layouts are refused before any patch is applied.
- Diagnostics: admission probe, latch bypass, budget unlock, VENC journal,
  daemon dump, capture snapshot and privacy mask.
- 6.72 diagnostic support.
- Per-firmware offset table (`tools/data/ps4_firmware_offsets.json`) where an
  unmeasured field stays unmeasured, so a zero can never pass for an offset.

### Streaming and viewer

- H.264 capture with 720p, 1080p and 1440p profiles in 60 fps mode; the PS5
  picks 1080p at runtime for an observed 1080p output mode and explicit
  arguments override that choice.
- HTTP streaming with RTP packetization, capture restart and discontinuity
  handling.
- The HTTP server no longer blocks the start when its default port is taken: it
  scans the next ten ports and logs which one it bound.
- A stalled viewer no longer poisons the RTP stream: the sender detects a
  backlog, asks for a keyframe and resumes from an IDR.
- Periodic keyframe refresh and improved jitter recovery.
- Built-in browser viewer and dashboard; the owner banner is opt-in
  (`PBS_WEB_LOGO=1`) and off by default, so `/banner.png` answers 404 in a
  default build.

### Build, CI and release

- `release.yml`: validate the version against `VERSION`, build both payloads,
  verify every downloaded asset against its `.sha256`, register the version in
  the Seregon Store and publish the GitHub release. A run without `publish`
  stops at a draft.
- `verify.yml`: offline PS4 regression on the host plus optional PS4/PS5
  cross-builds.
- SDK provisioning without developer-specific paths: a preinstalled SDK, a
  repository archive variable, or a shallow clone of the upstream SDK.
- PS5 ELF validation (`tools/verify_ps5_elf.py`) runs on every build.

### Known limits

- PS4 hardware capture is validated on 11.00 (180 s sessions, ~59.9 fps, decoded
  recording) and on 14.00 (fw `0x14008001`, 720p, runtime-discovered layout,
  Avcap Init/Open/Start `0x0`, HTTP streaming), both at 720p. Other PS4 firmwares
  remain unverified and PS4 audio is disabled.
- PS5 profiles are verified by offline disassembly and bounds checks; a console
  smoke test is still the last step for each release. 4.00 was promoted from
  experimental to measured on 2026-10-29 after its dump was re-checked
  instruction by instruction, and it has the same remaining step.
- Native 4K, 120 fps and zero latency are not supported claims. The 60 Mbit/s
  encoder budget is a ceiling, not a guaranteed bitrate.
