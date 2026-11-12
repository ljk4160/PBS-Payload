# Changelog

Notable changes to PBS Payload. `VERSION` at the repository root holds the
numeric version; a release tag may add a suffix (`v0.2.0-beta.1`).

# 更新日志（中文）

PBS Payload 的重要变更。仓库根目录的 `VERSION` 保存数字版本号；发布标签可以
带后缀（如 `v0.2.0-beta.1`）。

## v0.4.0-beta5-fix6.2（本 fork 的增强版）— 2026-11-12

### 修复：打开 `/log` 会把整个 payload 拖死

- 根因：定位“最后 N 行”用的是**逐字节** `fseek()` + `fgetc()` 回扫。日志长到
  500 KB（约 7 万行）时，一次 `/log?lines=300` 要执行几十万次系统调用，
  payload 的采集线程与 accept 线程一起被拖住，外部表现就是“打开日志插件直接崩”。
- 重写：一次读入文件尾部（最多 4 MB）到内存，在缓冲区里倒序找换行，发送后
  再按 250 ms 轮询追加内容；内存不足或读取失败时退化为只跟随新内容，不再回扫。
- 其余行为不变：`/log?lines=N`（默认 300，上限 5000）、10 分钟上限、客户端断开
  即结束、文件被截断自动重开。

## v0.4.0-beta5-fix6.1（本 fork 的增强版）— 2026-11-12

### 观看端超时自动断开

- 媒体连接（`/frames`）的发送超时从 5 秒收紧到 **2 秒**：一个连续 2 秒吃不进
  数据的观看端不可能恢复，直接断开并释放它的 worker 槽与编码器环形缓冲。
- 所有连接启用 **TCP keepalive**（idle 10 s、间隔 5 s、3 次探测 ≈ 25 秒判定死亡），
  合盖/掉 Wi-Fi 这类“没有 FIN 就消失”的观看端会被内核回收，不再一直占着
  `clients` 计数。
- 断开时在 `inject.txt` 写一行 `viewer detached (send failed or timed out)`，
  方便确认是哪个观看端被回收。
- 浏览器侧原本已有超时处理（watchdog 判定 stall 后 `controller.abort()` 并取消
  reader），这次补的是服务端那一半。

## v0.4.0-beta5-fix6.0（本 fork 的增强版）— 2026-11-12

### 槽占用时改为递增退避（30 s → 5 min）

- 现场统计（同一台主机、未重启）：19 次启动、**186 次 `Start() 0x81950008`**、
  17 轮 “slot busy after 5 attempts”。固定 30 秒重试等于持续触碰编码器，
  反而让槽更难释放。
- 现在按 30 s → 60 s → 2 min → 5 min 递增退避，第 4 轮起在日志写
  “the console needs a full power cycle, not a rest-mode wake”；`Start` 成功后
  序号清零。
- 结论：`0x81950008` 连续出现且重启后依旧，只能**完全关机再开机**（休息模式
  唤醒不会重建编码器）；退避只是避免 payload 继续加剧占用。

## v0.4.0-beta5-fix5.9（本 fork 的增强版）— 2026-11-12

### 重新注入改为“先礼后兵”，不再留下占用的编码器槽

- 事实澄清：`PBS replace: killed N previous instance(s)` 杀的是 **payload 自己的进程**，
  而编码器槽属于目标进程（SceRemotePlay）。`SIGKILL` 跳过了
  Stop/Close/ChangeState/Terminate，槽就留在目标进程里——**反复重注入本身就是
  把槽打满的原因**。
- 现在：注入时先对旧实例发 `SIGTERM`，最多等 2 秒让它在正常路径里释放采集，
  只有还活着才 `SIGKILL`；采集循环增加 `pbs_terminate` 检查，收到信号后跳出、
  走完整 cleanup 再退出，日志写 `terminated by signal; capture released`。
- 因此正常替换不再需要重启主机；只有旧版本留下的脏槽仍需重启一次。

## v0.4.0-beta5-fix5.8（本 fork 的增强版）— 2026-11-12

### 启动失败不再直接退出，改为持续等待编码器槽

- 现场日志：`OpenVideo ok` → `Start 0x81950008` 连续失败，
  5 次快速重试打完后 payload 退出，主机上从外面看就是“启动不了”。
- 现在：5 次快速重试（1 s / 1 s / 2 s / 2 s / 2 s）仍失败时，不再退出——
  打印 `Encoder slot busy after 5 attempts; retrying in 30 s` 并在主机上弹通知
  `PBS: encoder slot busy - will retry in 30 s`，然后每 30 秒重试一次。
  槽一旦被释放（游戏/Remote Play 退出），串流会自动接上，不需要重新注入。
- 若 10 分钟内一直失败，说明编码器槽真的死了，需要重启主机。

## v0.4.0-beta5-fix5.7（本 fork 的增强版）— 2026-11-12

### 修复：请求 720p 却打开 1080p、以及分辨率/码率控件永久变灰

- 现场 `config.json`：`width/height=1920x1080`，`requested_height=720`，
  `profile_changing=true` —— 说明切换标记被“锁死”，页面因此把
  分辨率/码率/应用全部禁用。
- 根因一：fix5.5 的“换 feature 槽”会把 720p 请求改成 1080p 打开；
  根因二：`pbs_h264_configure()` 只在 `profile_height == height` 时清除
  `profile_changing`，实际开在 1080p 时条件不成立，标记永久为真。
- 修复：
  1. 移除自动换档，请求什么分辨率就重试什么分辨率；
  2. `configure()` 无论实际开到哪个分辨率都结束切换状态，若与请求不符则记
     `profile_error`（页面显示“切换失败：请求 Xp，当前 Yp”）；
  3. `config.json` 增加 20 秒看门狗：`profile_changing` 卡住超过 20 秒自动
     判失败并解锁，控件不会再被永久禁用。

## v0.4.0-beta5-fix5.6（本 fork 的增强版）— 2026-11-12

### 音频改用 IMA ADPCM：每位观众 1536 → 384 kbps

- 采集到的 float32 立体声在 payload 内转 int16，再按 4 bit/样本做 IMA ADPCM，
  每帧打成一个字节 `(L<<4)|R`；48 kHz 立体声 = **384 kbps**（原来 s16 是 1536）。
- `/config.json` 的 audio 描述变为 `{"codec":"adpcm_ima","channels":2,"rate":48000,"bits":4}`；
  查看器新增 `adpcm_ima` 解码分支（同样的 IMA 表，状态跨 chunk 保持，音频重新连接时复位）。
- 回环测试（440 Hz + 3 kHz 复合信号，1 秒）：**SNR 40.3 dB**，属 4-bit ADPCM 正常水平。
- 保留降级开关：`audio16` 用 16 位 PCM（1536 kbps）、`audio32` 用原始 float32（3072 kbps）。

## v0.4.0-beta5-fix5.5（本 fork 的增强版）— 2026-11-12

### 编码器槽卡死：重试更久 + 换 feature 槽

- 现场日志：窗口内 26 次 `OpenVideo ok`、**25 次 `Start() 0x81950008`**、只有 1 次
  `Start` 成功，之后画面停在旧帧（status 的 fps 因均值稀释逐渐下降）。
- 重试从“2 次 / 1 秒”提升到 **5 次**：前两次间隔 1 秒，第 3 次起 2 秒；第 3 次时
  自动切换到另一个 feature 槽（720p ↔ 1080p）再试；`Start` 成功后计数复位，
  下次卡槽仍会重试。
- `/capture/apply` 增加去抖：已有 `restart_request` 挂起时返回 409
  “A restart is already pending”，避免连续点“应用”堆积重开把槽打满。

## v0.4.0-beta5-fix5.4（本 fork 的增强版）— 2026-11-12

### 音频改为 16 位 PCM：每位观众省下 1.5 Mbps

- 实测：720p60 视频 3.3 Mbps 的同时，**音频 PCM f32 占 3.08 Mbps**（48 kHz 立体声），
  几乎和视频一样多；4 个观众时上行要同时发 4 份。
- 采集到的浮点样本在上行前转成 `int16`，`/config.json` 的 audio 描述改为
  `pcm_s16le`（bits 字段同步为 16）；查看器本来就支持该格式解码，无需改动。
- 每位观众的音频带宽从 3.08 Mbps 降到 **1.54 Mbps**；需要原样浮点时用
  `avserve … audio32` 启动参数关闭。

## v0.4.0-beta5-fix5.3（本 fork 的增强版）— 2026-11-12

### 新接口：`GET /log` 实时查看 inject.txt

- `GET /log?lines=N`（默认 300 行，上限 5000）先回最后 N 行，随后每 250 ms
  轮询追加内容，等价于 `tail -f`；单次连接最长 10 分钟，客户端断开即结束。
- 支持文件被截断/替换：检测到尺寸回退就重新打开。
- 查看器页脚新增「日志」链接（新标签页打开），命令行等价：
  `curl -N http://<主机>:9999/log?lines=200`。

## v0.4.0-beta5-fix5.2（本 fork 的增强版）— 2026-11-12

### 修复：点一次“应用”后码率下拉永久变灰

- 原因：Apply 处理函数在开始请求时禁用三个控件，但只在 `catch` 分支恢复；
  成功路径没有人恢复，于是码率下拉（以及分辨率下拉、应用按钮）从此一直 disabled。
- 修复：改为 `finally` 恢复，并在 `refreshProfiles()` 里按 `profileBusy || recording`
  同步码率下拉的禁用状态（真正切换期间仍然禁用，切换完成后自动恢复）。

## v0.4.0-beta5-fix5.1（本 fork 的增强版）— 2026-11-12

### 码率档位重排；自动档才自适应

- 档位改为 **150 / 120 / 100 / 60 / 50 / 30 / 20 / 10 / 8 / 6 / 5 / 4 / 2 Mbps + 自动**；
  服务端上限由 100 Mbps 放宽到 150 Mbps。
- **只有“自动”会自降**：内部维护一个自适应上限（初始 20 Mbps），
  `Delay > 400 ms` 连续 5 秒降一档（间隔 ≥20 s，最多 3 次，不自动升）。
  选择任何固定档位后，页面不再改动码率。
- 每次点“应用”重置自适应计数；把下拉切回“自动”会清除自适应上限，从头开始。

### 关于“再压缩”的结论

- 采集走 `sceAvcap2`，编码固定 H.264；B 帧/前瞻、profile、QP 曲线都没有可控参数，
  能调的只有 **分辨率 / 帧率 / 码率 / QP 下限（18）**。
- 因此“同画质更省带宽”或“同带宽更高帧率”只能靠降低分辨率实现；HEVC/AV1
  需要换 Remote Play 通道（H.265）或主机编码硬件（AV1 不存在）。
- 传输侧已做的：TCP_NODELAY、64 KB 发送缓冲（避免 bufferbloat）、每观众积压上限
  （24 帧 / 低延迟 6 帧）、关键帧请求限流 1 次/秒。

## v0.4.0-beta5-fix5.0（本 fork 的增强版）— 2026-11-12

### 自适应码率：链路拥塞时自动降档

- 实测数据表明：高码率下 `Delay` 会爬到 600–1000 ms（链路排队），手动降到
  低码率后立刻回落到 4 ms。这一判断现在由页面自动执行。
- 规则：`Delay > 400 ms` 连续 5 秒 → 按 20/10/8/6/4/2 Mbps 顺序降一档并调用
  `/capture/apply`；两次降档间隔 ≥20 s，单次会话最多降 3 次，**永不自动升档**。
  降档后底部提示“网络拥塞，码率已自动降到 N kbps”，下拉同步更新。
- 说明 `Delay` 与 `Playback buffer` 是两个量：前者是帧从服务端发出到你收到的
  传输延迟，后者是浏览器里尚未播放的时长；调缓冲只改后者。

## v0.4.0-beta5-fix4.9（本 fork 的增强版）— 2026-11-12

### 修复 fix4.8 引入的重开死循环

- **问题**：fix4.8 在每次 `Start` 失败时都把重试计数重置为 2，于是“失败→重开→
  再失败→再重置”，变成每 1 秒无限重开（日志里出现连续的
  `Start slot busy; reopening after 1 s (retries left 1)`）。反复重建注入通道
  之后目标进程侧残留，后续注入直接 `begin failed errno=17`（EEXIST），表现为
  “彻底启动不了”。
- **修复**：改为 `start_slot_busy` 标记 + `start_retry_done` 计数，**每个进程最多
  重开 2 次**，用完即退出。
- **注入通道加退避**：`pbs_inject_begin` 返回 EEXIST 时间隔 0.8 秒重试，最多 3 次；
  仍失败则写 `injection unavailable: restart the console before retrying` 并退出，
  不再空转。

## v0.4.0-beta5-fix4.8（本 fork 的增强版）— 2026-11-12

### 启动失败修复：`Start() 0x81950008` 自动重试

- 日志形态：`OpenVideo() ok` 之后 `SetVideoBitrate` 与 `Start()` 都返回
  `0x81950008`（该 feature 的编码器槽仍被上一个正在退出的会话占用），随后整个
  payload 直接退出，表现为“注入后起不来”。
- 旧版有一层“失败换槽/重开”逻辑，在早前的 `git checkout` 回退中丢失。现在补回：
  检测到 `Start` 返回 `0x81950008` 后，**间隔 1 秒整体重开两次**（不是简单重试
  Start，而是重走注入→Initialize→OpenVideo 的完整流程），日志写
  `Start slot busy; reopening after 1 s (retries left N)`。
- 两次仍失败则按原逻辑退出——那说明槽状态真的卡死，需要重启主机。

## v0.4.0-beta5-fix4.7（本 fork 的增强版）— 2026-11-12

### 延迟：按观众限制积压，不再累积

- 每个观看端的发送线程过去只在 64 帧环形缓冲（≈1 s）被覆盖时才丢帧追赶，
  网络稍一抖动积压就长期停留在几百毫秒。现在按**积压上限**主动追赶：
  普通模式 24 帧（≈400 ms），低延迟模式 6 帧（≈100 ms），触发时跳到最新帧
  并请求一个 IDR。
- `/frames` 支持 `?lowlat=1`；切换“低延迟”开关会重连该流，让新的上限立即生效。
- 音频默认延迟 120 ms → 80 ms，与更紧的视频缓冲对齐。

## v0.4.0-beta5-fix4.6（本 fork 的增强版）— 2026-11-12

### 延迟：低延迟模式 + 实测延迟

- **低延迟开关**（dock 内，绿描边表示开启）：把跟播阈值从 1.2 s 收紧到 0.45 s，
  硬跳目标从 `end-0.25` 收紧到 `end-0.06`，落后 0.2 s 以上用 `playbackRate 1.15`
  追帧。默认关闭（平滑优先），状态存 localStorage。
- **实测延迟显示**：`/frames` 的帧头由 8 字节扩为 12 字节，新增服务端发送时刻
  （单调毫秒）；`/status.json` 增加 `now_ms`。查看器用一次往返估算时钟偏移，
  header 上显示 `Playback buffer: 120 ms · Delay: 186 ms`（帧到达时刻减服务端发送时刻）。

### 修复

- **码率上限恒被提交为 `kbps=0`**：`applyLang()` 在切换/初始化语言时用
  `innerHTML` 重建带 `data-i18n` 的 label，`<select id="bitrate">` 因此被替换成
  新节点，页面早先缓存的引用指向已脱离文档的旧节点，其 `value` 永远是 `0`。
  现改为只更新 label 的前导文本节点，控件本身不再被重建；Apply 处理函数也改为
  每次按 id 重新取控件值。

## v0.4.0-beta5-fix4.5（本 fork 的增强版）— 2026-11-12

### 控制台噪音清理

- `/banner.png` 在没有内嵌 banner 的构建里不再返回 404，改为 1×1 透明 PNG。
  查看器本来就会在 `onerror` 里移除该 `<img>`，但 404 会在每个浏览器控制台留下
  一条 failed request。
- 说明：音频仍使用 `ScriptProcessorNode`（浏览器会打印 deprecation 警告）。
  查看器走明文 HTTP，而 `AudioWorklet` 需要安全上下文（HTTPS/localhost），
  因此这里不能用它；换成 HTTPS 后才能切。

## v0.4.0-beta5-fix4.4（本 fork 的增强版）— 2026-11-12

### 固件 12.40（`0x12400009`）分辨率修复

- **1440p/2160p 在该固件上必定打不开**：`OpenVideo` 稳定返回 `0x81950008`
  （日志中 4 次 1440p 全部失败，同一台机器 1080p 可连续跑 469 秒 / 12.29 Mbps）。
- **失败后自动降 1080p 重试一次**：过去直接 `OpenVideo failure quarantined`，
  整个 payload 执行期不再尝试；现在记录失败尺寸并以 1080p（feature 12）重开一次，
  `inject.txt` 会写 `OpenVideo failed at 2560x1440; retrying once at 1080p`。
- **按主机 `videoOutMode` 钳制请求**：缓存最近一次读到的 `videoOutMode`，当它为
  `0x10`（主机只输出 1080p）时，网页选择 1440p/2160p 会被直接钳到 1080p，避免
  每次改码率触发的重开都撞进同一个失败。

## v0.4.0-beta5-fix4.3（本 fork 的增强版）— 2026-11-12

在 fix0.4 基础上继续的界面 / 输入 / 串流修复，全部改动在 12.00 实机验证。

### 串流与性能

- **码率上限真正生效**。`sceAvcap2` 的 rate group（rate / budget / initial / QP）
  在 `OpenVideo` 时写进请求参数，旧实现只调用 `sceAvcap2SetVideoBitrate`，输出
  速率完全不变（实测一直跑在 ~14.8 Mbps）。现在点「应用」时把上限写入请求并
  重开一次采集；`/config.json` 增加 `cap_kbps` 便于前端回显。
- **关键帧请求风暴修复**。显式 IDR 请求过去绕过 4 秒周期门，三个观看端每次掉队
  都请求一次，单次会话日志里出现 1527 次 `RequestKeyFrame`，低码率下形成持续性
  码率尖峰。现在显式请求也限 1 次/秒，客户端兜底阈值由 2 秒提高到 8 秒。
- **播放器跟播策略**。原先缓冲落后 300ms 就 `currentTime` 硬跳，抖动时一秒触发
  多次，解码器反复饥饿（表现为卡顿）。改为落后 >1.2 秒才硬跳，0.15–1.2 秒用
  `playbackRate` 1.03 / 1.08 平滑追赶。
- **音频 `0x81950008` 日志风暴**。失效会话过去每轮轮询打印一行（单会话 6.5 万
  行）。现在同一错误码只记一行，连续 120 次失败后重开一次音频通道，仍失败则停止
  发起 RPC。

### 界面

- **真全屏**：改为对 `<main>` 调用 `requestFullscreen()`，覆盖浏览器外壳与任务栏；
  全屏时隐藏所有控件，鼠标移动才浮出控制条，静止 2.2 秒自动隐去；ESC 与 Ctrl+Alt
  均可退出。
- **dock 重排**：两行布局，统一 36px 控件高度；顺序为
  「分辨率 → 码率上限 → 应用」，点应用才生效（单次请求 `/capture/apply`，
  只重启一次采集）；分辨率选择器移到「保存截图」右侧。
- **按键说明卡**：新增在「录制存储」上方的键位说明（中英双语），含 PS / 截屏 /
  选项等全部映射。
- **移除编码选择器**：`libSceAvcap2` 没有任何编码格式入口（12 个候选符号实测全部
  不存在），H.265 无法在该采集链路实现，故去掉无作用的灰色下拉。

### 输入

- **键盘不再依赖焦点**：连接后键盘与手柄全时段可用；PS5 映射在 capture 阶段吞掉
  空格 / 方向键 / 回车 / Backspace，播放器的播放暂停、音量等热键彻底失效。
- **单按钮状态切换**：「连接手柄」↔「断开手柄」由同一个按钮表达，删除全部提示文案。
- **虚拟手柄**：画面内叠加半透明手柄（L2/L1/R1/R2、十字键、△○✕□、L3/R3、
  双摇杆、截屏 / PS / 选项），按键走 `PBSController.virtual` 直接下发；按钮仅在
  手柄连接后显示，断开自动关闭并隐藏。

### 系统

- **电源控制**：新增 `POST /system/{standby,reboot,poweroff}`，dock 增加
  待机 / 重启 / 关机按钮（先回 ACK，再延迟 400ms 调用 Sony API，避免网络先断）。
  重启走 `sceSystemServiceRequestReboot`，关机优先 `sceSystemStateMgrTurnOff`，
  待机走 `sceSystemStateMgrEnterStandby`（dlsym 解析，缺符号不崩）。

### 修复

- 默认采集档改为 720p（feature 12），注入后不再默认 1440p。
- 分辨率切换后的空档期不再让 `applyLang` / 播放器跟进逻辑抛异常。
- 全屏右侧黑竖条：`main` 的 `max-width` / `padding` 与 `scrollbar-gutter` 在真全屏
  下仍生效，已全部复位为全屏尺寸。

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
