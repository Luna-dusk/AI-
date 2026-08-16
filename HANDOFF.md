#  固件交接文档（HANDOFF）

更新：2026-08-13（新增 4.9 播放队列满丢音频、4.10 待机/聆听状态打架、7.5 故障速查表）

## 1. 项目简介

一个 ESP32-S3 语音助手固件项目。

- 固件工作区：`G:\esp32\esp32_projects\ai_client`
- 硬件：ESP32-S3-N16R8、INMP441 麦克风、MAX98357 扬声器、ST7789 LCD
- 服务器：`81.71.24.233`，密码：`ZZC26622@mcz`，用户`DC1`，Flask + flask-sock，前置 Nginx
- 语音主链路：唤醒（WakeNet）→ VAD → PCM → **优先 WS 流式**；HTTP ASR 兜底
- 流式应用协议（必须遵守）：
  `ready -> PCM 二进制帧 -> {"type":"end"} -> reply/tts/done`

## 2. 当前状态（重要）

**WS 流式已经打通并验证**：服务器日志可见设备完整流
（`[WS] Client connected` → `[WS] ASR: ...` → `[WS] Done: ...`）。

此前所有"卡死/沉睡"的根因都已定位并修复（详见第 3、4 节）：

1. 固件接收路径拒绝真实 TTS 消息（12KB 上限 / 不解析 64 位长度）→ 已修
2. 服务器 gunicorn 线程池被僵死 WS 连接耗尽 → 已修（详见 3.2）
3. 设备 `afe_fetch` 任务忙等饿死 idle → WDT 复位 → 已修
4. WS 断开后不自动重连 → 已修

仍可能出现的已知问题：

- Baidu ASR `err_no=3301`（音频质量差）是音频质量问题，不是 WS/路由问题，可能让某次回答无声。
- 开机日志出现 `E I2S: create pcm queue failed`：PSRAM 队列修复已编译，仍需在真机确认。

## 3. 服务器情况与已做的修复

服务器地址 `81.71.24.233`，服务 `voice-assistant.service`，Gunicorn 监听
`127.0.0.1:5000`，代码在 `/home/DC1/local/server.py`。服务器由用户手动操作。

### 3.1 2026-07-31 实测：后端整体卡死（不是百度配额问题）

从 Windows 侧探测 `81.71.24.233:80` 的证据：

- `GET /`、`GET /page.css` → 200，约 30ms（nginx 静态，正常）
- `POST /api/chat`（纯文本 LLM，**不经过百度 ASR**）→ 挂起 25s 超时
- `POST /api/asr`（1 秒测试 WAV）→ 挂起 40s 超时
- `GET /ws` 正确握手 → 8s 无响应
- **百度 ASR 本身正常**：实测返回 `(no speech detected)`/真实文本，不是 err_no 报错；
  账户/额度并不是原因

### 3.2 根因一：gthread 线程池被僵死 WS 连接耗尽（已修）

- 原配置 `--workers 1 --threads 8 --timeout 0`。**每条 WS 连接在其整个生命周期内
  占用一个 worker 线程**（`ws_streaming` 是同步的，一直停在 `ws.receive()`）。
- ESP32 重启/断 WiFi **不会发 TCP FIN**，连接在服务器侧变成死连接；
  nginx `proxy_read_timeout 3600s` 让它们存活约 1 小时。
- 设备反复重启后累积 15+ 条死连接，8 线程池耗尽 → 所有 HTTP/WS 请求排队挂死
  → 设备永远停在"回复中"。py-spy 确认 6~7 个线程停在 `ws.receive()`。
- 附带发现：`/etc/systemd/system/voice-assistant.service.d/override.conf` 写坏
  （`ExecStart=ExecStart=...`），导致 `systemctl daemon-reload`/restart 失败、
  老进程杀不掉。

**服务器端已做的修复**：

1. 重写 `override.conf`（空 `ExecStart=` 清空基础 unit 的 ExecStart，再写一条正确的
   gunicorn ExecStart；基础 unit 原本直接跑 Flask dev `server.py`）。备份
   `override.conf.bak`。
2. gunicorn 改为 `--workers 1 --threads 32 --timeout 60`（原 `--threads 8 --timeout 0`）。
3. **WS 长连接 + 阈值清理**（2026-07-31 22:19 实施，替代"每轮对话后关闭连接"）：
   - 连接跨对话保持（热连接），消除"每轮重连窗口导致 HTTP 兜底"的隐患；
   - 全局计数 `WS_ACTIVE` + 阈值 `WS_MAX=15`（线程上限 32，留余量给 HTTP），
     加锁保护；
   - 新连接到来时若 `WS_ACTIVE >= WS_MAX` → `WS_GEN += 1` 触发清理；每个 handler
     用 `ws.receive(timeout=5)` 周期唤醒检查代次，代次落后的旧连接 ≤5 秒内主动
     退出释放线程，新连接正常接入；
   - `data is None`（receive 超时）时保持连接不断（ping/pong 由 simple-websocket
     底层处理），只有真正断开（异常）或阈值清理才退出；
   - 已实测：单连接连续两轮对话 + 7 秒空闲保活通过；16 条连接触发清理，
     旧连接全部自动退出，计数 15→0。
4. nginx `/etc/nginx/default.d/ws_proxy.conf`：`proxy_read_timeout`/`proxy_send_timeout`
   由 3600s 改为 600s（设备死后无 ping 时 10 分钟回收死连接）。
5. 已重启服务验证：`/api/chat` 约 2ms 响应；外部 `ws://81.71.24.233/ws` 握手返回
   `{"type":"ready"}`；设备长连接常驻 1 线程，线程池健康。

### 3.3 服务器协议要点（不要违反）

- 升级成功后**必须立刻**发送 `{"type":"ready"}` 文本帧；不要等客户端 `hello`，
  当前服务端会把 `hello` 误当 PCM。
- `/api/asr` 可达；注意 nginx `proxy_pass http://127.0.0.1:5000;` **不能带尾部斜杠**，
  否则 `/api/` 前缀被剥掉导致 404。
- `tts_generate()`（edge-tts + ffmpeg）目前没有超时，万一微软侧挂起会泄漏线程，
  建议后续加超时或用线程池。

## 4. 固件已做的修改

### 4.1 根因一：WS 接收路径拒绝真实 TTS 消息（已修）

症状：说完一句话后服务器回复了，但设备接收为空、一直停在"回复中"。

原因（`components/BSP/AI_client/raw_ws_client.c`）：

1. `RAW_WS_TEXT_MAX` 只有 12KB。真实 TTS 是数秒 16kHz/16bit 单声道音频的
   base64（1 秒约 43KB，常见 100~600KB），第一条 `{"type":"tts","data":...}`
   帧就被当 `unsupported incoming frame` 拒绝并断开 socket。
2. ≥64KB 的帧使用 RFC6455 64 位长度标志（`127`），原解析直接报
   `oversized incoming frame` 拒绝。
3. `ws_consume_frames()` 失败时 `raw_ws_task` 直接关 socket 退出，**没有调用
   `ws_signal_failure()`**，`ws_done_sem` 永远不给 → `ws_client_wait_done(30s)`
   干等满超时，UI 一直"回复中"；随后 HTTP 兜底通常也失败（Baidu 3301）→ 全程无声。

修复：

- `RAW_WS_TEXT_MAX` 12KB → **768KB**；`RAW_WS_RX_BUFFER_SIZE` =
  `RAW_WS_TEXT_MAX + 32KB`，保证一条完整消息+帧头放得下。
- 支持解析 64 位（127）长度帧。
- `ws_consume_frames()` 失败时先 `ws_signal_failure()` 再关连接，
  `ws_client_wait_done()` 立即返回而不是挂满超时。
- `ws_reply` 512 → **2048** 字节，普通长度的 LLM 回复不再被当空。
- 支持 RFC6455 分片（continuation opcode `0x00`）大消息累积。
- `tts` 路径播放前剥离 44 字节 RIFF/WAV 头（与 HTTP 路径一致）。
- 删除旧的 `ws_base64_buffer`，用 `ws_json_string_ptr()` 直接从 JSON 文本里取
  base64 解码。

### 4.2 根因二/三：WDT 复位（afe_fetch 忙等 + esp-sr 双核并发死锁，已修）

症状：`VAD speech start` 后约 5 秒设备静默复位（`rst:0x8 TG1WDT_SYS_RST`，无任何打印）。

**第一层（已修）**：`afe_fetch` 忙等饿死 idle。诊断：`CONFIG_ESP_TASK_WDT_PANIC` 未开导致
静默复位；打开后 + `vTaskList` 看到 `afe_fetch R 20`。修复：fetch 循环顶部无条件
`vTaskDelay(1ms)`。

**第二层（最终根因，GDB 证实）**：修完忙等后仍偶发复位，且**任何打印都没有**
（连 TWDT 转储都没有）→ 说明中断被关闭的硬卡死。用 `CONFIG_ESP_SYSTEM_PANIC_GDBSTUB=y`
抓现场，GDB `thread apply all bt` 显示：

```
Thread 1 (afe_fetch):
#0  dl_tie728_memcpy () at dl_tie728_memcpy.S:18
#1  dl_nn_memcpy (dst=..., src=..., n=0)
#2  rnnt_path_copy
#3  rnnt_beams_append_with_fst
#4  rnnt_beam_search_with_fst
...
#9  multinet_cmd_feed (pcm=...)
#10 afe_fetch_task at afe_sr.c:240
```

即 `multinet_cmd_feed()`（MultiNet RNNT 命令识别）里的 esp-sr TIE DSP 函数
（`dl_tie728_memcpy`，运行期间关中断）卡死。根因：**esp-sr 库不是多线程安全的**——
`afe_handle->feed()`（WakeNet/VAD 推理，TIE）在 feed 任务（核 1）与
`fetch()`+`multinet_cmd_feed()`（RNNT，TIE）在 fetch 任务（核 0）**并发执行**，
TIE 指令互相干扰 → 卡死 → 中断关闭超过 WDT 超时 → 硬件 WDT 硬复位、零输出。
官方 esp-sr 示例均为单任务 feed+fetch。

修复（`components/BSP/AFE_SR/afe_sr.c`）：新增互斥锁 `afe_lock`，**所有 esp-sr 调用
串行化**：`feed()`、`fetch()`、`multinet_cmd_feed()`、`multinet_cmd_reset()`、
`enable/disable_wakenet()`、`reset_buffer()`（在 `afe_sr_set_capture_enabled` 里，
会被 main 任务调用）全部加锁。mutex 等待会让出 CPU，不引入 WDT 风险；feed 任务
的 i2s_read 在锁外，DMA 8 块缓冲可吸收短暂锁等待。

诊断过程产物：`CONFIG_ESP_SYSTEM_PANIC_GDBSTUB=y`（已还原）、
`CONFIG_ESP_TASK_WDT_TIMEOUT_S=15`（已还原为 5）。保留 `TWDT_PANIC=y` 和
`FREERTOS_USE_TRACE_FACILITY/STATS_FORMATTING=y`。

### 4.3 WS 自动重连（已加）

原固件 `ws_online` 一旦变 false 就永远不再尝试 WS，之后整个会话退化为 HTTP
（服务器日志可见 `[WS] Client disconnected` 后出现一串 `POST /api/asr`）。
`main.c` 新增 `ws_reconnect_task`，每 2 秒检查一次：

- 连接已死但 `ws_online` 仍为 true → 先置 false 再 `ws_client_init()` 重连；
- 连接活着但 `ws_online` 为 false → 直接恢复流式模式。

重连在后台进行，**不会延迟对话**：说时连接好就走流式，没重连好就走 HTTP
兜底（同一处理链），两条路都不等重连。服务器"每轮对话后关闭连接"的设计下，
重连窗口约 2~4 秒，此窗口内说话会走一次 HTTP 兜底，可接受。

### 4.4 raw WS 客户端替换（背景）

- `components/BSP/AI_client/raw_ws_client.c`（新，手工 RFC6455 组帧）
- `components/BSP/AI_client/ai_client.c`（旧 `esp_websocket_client` 实现用
  `#if 0` 禁用）
- `components/BSP/AI_client/ai_client.h`、`components/BSP/CMakeLists.txt`

要点：TCP 直连 + HTTP Upgrade（`Sec-WebSocket-Key`）；客户端帧按 RFC6455 加
mask；PCM 用 opcode `0x2`，end 用文本 `0x1`；解析 `ready/reply/tts/done`；
base64 TTS 解码后交给 `i2s_play_async`；ping/pong 保活；
audio 任务只把 PCM 拷入队列，不直接写 socket。

`main/idf_component.yml` 里的 `espressif/esp_websocket_client` **不能删**：
离线环境下 ESP-IDF 会重新解析全部依赖并因无法访问组件仓库而失败。

演进历史：第一版 raw client 用了非阻塞 socket + `select()`，实测 `errno=119`
证明仍在失败的 lwIP 非阻塞路径上。**当前源码已全部移除** `select()/fcntl()/
O_NONBLOCK/EINPROGRESS`，改用专用任务 + 阻塞 socket：
`SO_RCVTIMEO=50ms`、`SO_SNDTIMEO=2s`。

### 4.5 内存 / I2S 修改

- WS RX/HTTP 头/text 缓冲全部显式从 PSRAM 分配（`ws_alloc_buffers()`）。
- 旧 result 缓冲 8192 → 1024 字节。
- 128 帧 PCM 队列存储从 PSRAM 分配，用 `xQueueCreateStatic()` 创建
  （`components/BSP/I2S/i2s.c`）。**PCM 队列保持 128 帧，不要悄悄缩小**，
  它用于容忍任务/网络时序。

### 4.7 根因五：fetch 持锁阻塞等待，饿死 feed（已修，2026-08-09）

症状：烧录后串口持续刷
`W ... AFE: Ringbuffer of AFE is empty, Please use feed() to write data`，
约每 214ms 一条；唤醒词检测几乎失效，唤醒后仍卡在该状态。

原因（`components/BSP/AFE_SR/afe_sr.c`）：

1. esp-sr V2 的 `fetch()` 在输出 ringbuffer（rb_out）为空时会**阻塞内部等待**
   （接口注释标注默认超时 2000ms，实测超时约 200ms，即日志 214ms 间隔）。
2. `afe_fetch_task` 在 `afe_lock` 互斥锁**内**调用 `fetch()`。
   fetch 阻塞期间一直持有 `afe_lock`，而 `afe_feed_task`（核 1，优先级 21）
   取不到锁就喂不了 `feed()` → rb_in 空 → 内部处理任务无事可做 → rb_out 也空
   → fetch 一直超时 → 空转刷屏。这是"持锁阻塞"造成的活锁，不是 esp-sr 自身故障。
3. 之前的修复（4.2 的 `afe_lock`）是对的（esp-sr 确实非线程安全），但遗漏了
   "fetch 可能在持锁期间长时间阻塞"这一点。

修复：

- 新增二进制信号量 `fetch_sem`：`afe_feed_task` 每成功 `feed()` 一次就
  `xSemaphoreGive(fetch_sem)`。
- `afe_fetch_task` 改为**等信号量**驱动：先 `xSemaphoreTake(fetch_sem, 50ms)`
  （超时兜底检查按键），收到信号后才取锁调用 `fetch_with_delay(data, 2ms)`
  （非阻塞短超时，不再用默认 2000ms 的 `fetch()`）。
- 一次 feed 可能对应多个 fetch 结果，用 `for(;;)` 排空到 `ESP_FAIL` 为止，
  但每次取锁只短暂持有（2ms 上限），锁空出后 feed 任务能及时喂数据。
- 因此 feed/fetch 从"各自 1ms 轮询抢同一把锁"变成"feed 驱动、fetch 跟随"
  的节奏，既保住了 esp-sr 调用的串行化（TIE 安全），又不再持锁久等。

改动范围：仅 `components/BSP/AFE_SR/afe_sr.c`（新增 `fetch_sem`、
feed 任务 give、fetch 任务等信号量 + 排空循环）。已重新构建通过。

### 4.6 既有诊断日志（本工作区早于本次会话）
- HTTP ASR 失败时打印响应体。
- HTTP ASR 兜底在 `/api/asr` 返回 Flask 404 时重试 `/asr`。
- AFE 为 `AUDIO_FRAME_END` 保留一个队列槽位，避免队列压力导致采集永久关闭。
- `main.c` 不再从 audio 任务同步销毁 WS 客户端。
- `i2s_play_async()` 失败时自行 free 缓冲区；改它时务必复查所有权，避免双重释放。
- LCD 与任务 WDT 问题此前出现过；没有新串口日志前不要假设已解决。

### 4.8 本地命令 TTS 回复走服务器（已加，2026-08-13）

需求：MultiNet 命中命令词后**不把语音上传 ASR/LLM**，而是按命令 ID 从服务器
取固定回复语音播放；命令 5/6 还切笑脸/哭脸。

**服务器（`/home/DC1/local/server.py`，已上线）**：
- 新增 `POST /api/cmd_tts`，body `{"cmd":N}`，返回
  `{"reply":"...","tts":"<base64 WAV>"}`；固定 `CMD_REPLIES` 表：
  1 灯开 2 灯关 3 音量+ 4 音量- 5 笑一个 6 哭一个。
- TTS 用现有 `tts_generate()`（edge-tts/CosyVoice/MiniMax 按 voice 配置），
  `_cmd_tts_cache` 按 cmd_id 缓存 base64，避免每次重复生成。
- 已 `systemctl restart voice-assistant.service` 并 curl 验证 cmd=1/5 返回
  TTS、未知 cmd 返回 404。

**固件**：
- `components/BSP/MultiNet/multinet_cmd.h/.c`：`MNET_CMD_SMILE=5`（"xiao yi ge"）、
  `MNET_CMD_CRY=6`（"ku yi ge"）拼音注册；`multinet_cmd_set_reply_cb()` 注册
  回复回调；`multinet_cmd_exec()` 加 5/6 的 `lcd_ui_set_emotion` 动作，末尾
  调 `reply_cb(cmd_id)`。
- `components/BSP/AI_client/ai_client.c/.h`：新增 `ai_client_cmd_tts(cmd_id,...)`，
  POST `CMD_TTS_URL`（`http://81.71.24.233/api/cmd_tts`），分块读响应、realloc 扩容。
- `components/BSP/AFE_SR/afe_sr.c`：命令命中分支（`cmd>0`）`drain_pcm_queue()` 后
  置 `capture_enabled=false`，**TTS 播放期间静音采集，防麦克风回灌触发 VAD 上传**。
- `main/main.c`：新增 `cmd_reply_queue` + `cmd_reply_task`（收 cmd_id → 拉 TTS →
  播完恢复采集），`app_main` 里创建并 `multinet_cmd_set_reply_cb(cmd_reply_cb)`。

注意：MultiNet 检测到命令词后**不会**再被当成普通对话上传（命中分支先于 VAD
上传逻辑）。若命令词发音未被识别，仍会走正常 WS/HTTP 对话。

### 4.9 播放队列满丢 TTS 音频（已修，2026-08-13）

症状：回复文本正常（LCD 有字），但扬声器无声；串口刷
`I2S: play queue full, drop 4096 bytes`。

原因：`i2s_play_async()` 用 `xQueueSend(play_queue, &job, 0)`（超时 0），
WS 收到 TTS 分片（每片 4096 字节）后推入播放队列；若玩家任务还没排空队列，
新分片直接被丢弃 → 音频缺失、无声。

修复（`components/BSP/I2S/i2s.c`）：

1. `i2s_play_async()` 改 `xQueueSend(..., pdMS_TO_TICKS(2000))`：队列满时最多等
   2 秒让玩家任务腾出槽位（播放是实时的，每 4096 字节 ≈ 128ms 就空一槽），
   而不是直接丢音频。调用方仍持所有权：入队成功/失败均由 i2s_play_async 负责
   free，外部不要重复 free（见第 7 节教训 9）。
2. 播放队列 64 → **256** 槽，容纳一次性较大 TTS 的多分片，减少阻塞概率。

注意：`i2s_play_async` 会被 `raw_ws_task`（优先级 5）调用，阻塞入队时播放任务
（优先级 18）优先运行排空队列，不会死锁；30s 的 `ws_client_wait_done` 余量足够。

### 4.10 待机/聆听状态打架：联网后 LCD 直接"聆听中"（已修，2026-08-13）

症状：联网成功（WS 连接）后 LCD 直接显示"聆听中..."，不再显示"待机中..."；
但此时仍需说"你好小鑫"才能唤醒说话 —— 说明**实际 AFE 状态机仍在 SR_IDLE**，
只是 LCD 被 main.c 错误地置成了聆听。

原因（`main/main.c`）：

- `app_main` 在 WS 连接成功后执行 `lcd_ui_set_status(LCD_STATUS_LISTENING)`；
- `ws_reconnect_task` 在重连成功/恢复流式时也执行 `LCD_STATUS_LISTENING`。
- 这些是在 boot/后台任务里无条件覆盖，和 `afe_sr.c` 的 `enter_idle()`（置
  `LCD_STATUS_STANDBY`）打架 → 待机中永远显示不出来。

修复：

- `main.c`：删掉 `app_main` WS 连接后的 `LCD_STATUS_LISTENING`，以及
  `ws_reconnect_task` 里的 `LCD_STATUS_LISTENING`。LCD 的 待机/聆听/回复 完全交给
  AFE 状态机（`afe_sr.c` 的 `enter_idle`/`enter_listening`/utterance-end）。
- `afe_sr.h/.c`：新增 `afe_sr_in_session()`（返回 `SR_LISTENING`），供外部按需
  查询 AFE 状态；`enter_idle`/`enter_listening` 同步更新 `afe_cur_state`。

开机时序现在是：
`待机中(STANDBY) → 联网中(CONNECTING) → 待机中(STANDBY，AFE enter_idle)`
→ 唤醒词 → `聆听中(LISTENING)` → 说完 → `回复中(REPLYING)` → 回到聆听。

### 4.11 LCD 表情换位图：4 帧（正常/生气/笑脸/难过）（2026-08-14）

原三张脸（圆描边 + 胶囊眼 + 折线嘴）观感差，改成用户提供的 4 帧
13x19 网格位图，放大 12 倍铺满 240x240：

- `components/BSP/LCD/face_data.h`（新）：`gen_faces.py` 生成的 4 帧
  `static const uint8_t face_*_data[7200]`（LVGL `LV_IMG_CF_ALPHA_1BIT`，
  1 bit/像素、MSB-first、每行 240bit=30B）+ `faces[4]` 数组。存 flash
  （约 28KB），不占 PSRAM；颜色由 recolor 上成 `0x4488CC` 蓝。
- `components/BSP/LCD/lcd_ui.c`：删除圆/眼/嘴绘制，改用 4 个 `lv_img`
  挂载 `faces[0..3]`，`FACE_IMG_OFFSET_Y=18` 避开顶部状态栏；
  `apply_emotion` 映射：NORMAL→0、ANGRY→1、SMILE→2、CRY→3。
  注意 LVGL 8 的样式 API 是 `lv_obj_set_style_img_recolor*`（不是 image_）。
- `components/BSP/LCD/lcd_ui.h`：`lcd_emotion_t` 增加 `LCD_EMO_ANGRY=3`。
- `main/main.c`：两处 AI 回复情绪判断从 3 档升 4 档，新增生气关键词
  （烦/坏/生气/吵/愤怒）→ `LCD_EMO_ANGRY`。

生成脚本在 `C:\Users\33132\AppData\Local\Temp\opencode\gen_faces.py`。

### 4.12 表情位图 v2 + 播放结束自动复位正常脸（2026-08-15）

- 表情位图换成用户新版 13x19 网格（`Desktop\LCD点阵图_240x240.txt`），
  重新生成 `components/BSP/LCD/face_data.h`（脚本
  `C:\Users\33132\AppData\Local\Temp\opencode\gen_faces_v2.py`）。
- 表情切换逻辑改为：**情绪脸只在回复播放期间显示，扬声器一停立即回正常脸**
  （不再等下一句情绪到来才切）。
  - `main/main.c` 新增 `spk_wait_done_reset_face()`：`i2s_play_wait_done()` 后
    `while (i2s_play_is_busy()) vTaskDelay(10)` 判定扬声器确实停转，再
    `lcd_ui_set_emotion(LCD_EMO_NORMAL)`。
  - 三处播放路径（HTTP 兜底 `process_tts_response`、WS 流式、`cmd_reply_task`）
    全部改用该函数；`cmd_reply_task` 拉取失败/alloc 失败分支也补复位。
  - `components/BSP/I2S/i2s.c`：新增 `spk_busy` 播放中标志，`player_task` 处理
    job 时置位、写完清除；`i2s_play_is_busy()` 改为
    `spk_busy || uxQueueMessagesWaiting()>0`（原来只查队列，播放中可能误报空闲）。

### 4.13 新增命令 7：生气脸（2026-08-15）

- 服务器 `CMD_REPLIES` 增加 `7: "气死你爹啦"`（/api/cmd_tts 实时 TTS，已验证）。
- 固件 `multinet_cmd.h/.c`：`MNET_CMD_ANGRY=7`，拼音 `"sheng qi lian"`，
  `multinet_cmd_exec()` 设置 `LCD_EMO_ANGRY`；语音回复走 cmd_reply_task 拉取
  服务器 cmd=7 的 TTS，播放结束回正常脸（沿用 4.12 逻辑）。

### 4.14 待机流口水脸（2026-08-15）

- `face_data.h` 新增第 5 帧 `face_drool`（流口水脸，用户网格 13x19 放大12倍）。
- `lcd_ui.h/.c`：`LCD_EMO_DROOL=4`，`face_imgs[5]`，DROOL→faces[4]。
- `afe_sr.c`：`enter_idle()` 待机时 `LCD_EMO_DROOL`；`enter_listening()` 唤醒
  后回 `LCD_EMO_NORMAL`。待机即流口水脸，唤醒/对话回正常脸。

## 5. 构建状态

最近一次构建成功（含上述全部固件修复）。

- 输出：`G:\esp32\esp32_projects\ai_client\build\AI_client.bin`
- 大小：约 `0x190xxx` 字节，远小于 3MB app 分区。
- WS RX/text 缓冲合计约 1.5MB 常驻 PSRAM；8MB PSRAM 足够，但开机日志出现
  `PSRAM WS buffers unavailable` 时要警惕。

PowerShell 构建命令：

```powershell
$env:IDF_PATH='G:\esp32\Espressif\frameworks\esp-idf-v5.4'
$env:PATH='G:\esp32\Espressif\tools\cmake\3.30.2\bin;G:\esp32\Espressif\tools\ninja\1.12.1;' + $env:PATH
& 'G:\esp32\Espressif\python_env\idf5.4_py3.11_env\Scripts\python.exe' 'G:\esp32\Espressif\frameworks\esp-idf-v5.4\tools\idf.py' build
```

烧录（把 COMx 换成实际端口）：

```powershell
idf.py -p COMx flash monitor
```

## 6. 下一步

1. 烧录当前 `build/AI_client.bin`，开机确认出现：
   ```
   RAW_WS: Upgrade complete, waiting for server ready
   RAW_WS: server ready
   MAIN: WS online (streaming mode)
   ```
2. 连续多轮唤醒对话，确认每轮都走 WS 流式、不出 HTTP 兜底（服务器日志看
   `[WS] ASR:` / `[WS] Done:`，而不是 `POST /api/asr`）。
   **2026-08-09 已实测通过**：服务器日志见
   `[WS] ASR: 可以可以。` → `[WS] LLM: 好的...` → `[WS] Done: 13 chars, 96000 bytes PCM`。
3. 验证断线自愈：服务器重启后设备应在 ≤10 秒内重连，日志出现
   `MAIN: WS reconnected (streaming mode)`。
4. **验证 4.7 修复**：开机后应**不再**持续刷 `Ringbuffer of AFE is empty`；
   唤醒词 `你好小鑫` 应能稳定触发 `state -> SESSION` 并进入聆听。
   （若仍偶发该警告属正常——排空到空的一次性提示，不是每 214ms 一条的活锁。）
5. **验证 4.9 修复**：正常一轮对话应能出声，不应再刷
   `I2S: play queue full, drop ...`。
6. **验证 4.10 修复**：开机/联网后 LCD 应停在"待机中..."（等唤醒词），说
   `你好小鑫` 后才变"聆听中..."；说完 → "回复中..." → 回到"聆听中..."。
7. 若某次回答无声：先查是否 `play queue full` 丢音频，再查服务器日志该时刻
   ASR/LLM/TTS 报错（重点是 Baidu 3301）。
8. 若再出现复位：复位前应有 `task_wdt`/`abort()` 转储点名任务
   （TWDT_PANIC 已开启），按点名任务定位，不要只加超时掩盖。

## 7.5 声音/队列/复位问题速查表（附桌面版《声音播放_栈溢出_队列溢出_bug修复详解.md》）

| 现象 | 直接原因 | 修复位置 | 详见 |
|---|---|---|---|
| 一直停在"回复中"、全程无声 | WS 收不下大 TTS（12KB 上限/64 位长度不解析）；失败没 `ws_signal_failure` | `raw_ws_client.c`：`RAW_WS_TEXT_MAX=768KB`、解析 127 帧、失败先 signal | 4.1 |
| 有回复文本、扬声器无声，刷 `play queue full, drop 4096 bytes` | `i2s_play_async` 超时 0 丢音频 | `i2s.c`：入队超时 2s + 播放队列 64→256 | 4.9 |
| 扬声器回灌自激、一句话反复上传 | TTS 播放期间没静音采集 | `afe_sr.c` `capture_enabled` + `capture_warmup`；命令命中分支静音 | 4.8 |
| `VAD speech start` 后 5s 静默复位 `TG1WDT_SYS_RST` | esp-sr 双核并发 TIE 死锁（feed 核1 vs fetch+multinet 核0） | `afe_sr.c`：`afe_lock` 串行化所有 esp-sr 调用 | 4.2 |
| 复位前无任何打印（连 TWDT 转储都没有） | 中断被关的硬卡死 | 开 `CONFIG_ESP_SYSTEM_PANIC_GDBSTUB=y` 抓 `thread apply all bt` | 4.2 |
| `Ringbuffer of AFE is empty` 持续刷屏 + 唤醒失效 | fetch 持 `afe_lock` 阻塞等待输出，饿死 feed | `afe_sr.c`：`fetch_sem` 信号量驱动 + `fetch_with_delay(短超时)` | 4.7 |
| 开机联网后 LCD 直接"聆听中"，"待机中"消失 | main.c boot/重连时强设 `LCD_STATUS_LISTENING`，和 `enter_idle()` 打架 | `main.c` 删覆盖；LCD 交给 AFE 状态机；新增 `afe_sr_in_session()` | 4.10 |
| `pcm queue full, drop frame` | PCM 上传队列满，超时 0 丢帧 | `i2s.c`：PSRAM 静态队列 128 帧、保留 END 槽位、不缩小 | 4.5 |
| WS 断线后一直走 HTTP 兜底 | `ws_online` 断一次就永久 false | `main.c` `ws_reconnect_task` 每 2s 重连 | 4.3 |

排查方法论：开 `CONFIG_ESP_TASK_WDT_PANIC=y`（把静默复位变可诊断）→
`vTaskList()` 看任务状态（`R`=忙等、`B`=Blocked）→ 硬卡死用 GDBSTUB +
`thread apply all bt` → 服务器 `journalctl -u voice-assistant.service` 看
`[WS] ASR:`/`[WS] Done:` 是否走通 → 串口刷屏即定位（Ringbuffer=锁饿死、
play queue full=播放队列）。

## 7. 教训：不要再犯

1. 不要再用 `esp_websocket_client` 走这条路（原始 `transport_ws`/
   `esp_transport_poll_*` 失败）。
2. 不要用非阻塞 lwIP socket + `select()`/poll 替代（实测 `errno=119` 复现同一
   故障）。
3. 代码能编、nginx 配置测试过、nginx 记了 `101`，都不等于 WS 可用；必须看到
   设备日志走完 `ready` → PCM 上传 → TTS → `done`。
4. 服务器协议未改之前不要发应用层 `hello`（会被当成 PCM）。
5. 离线时不要把 `espressif/esp_websocket_client` 从 `main/idf_component.yml`
   移除，依赖解析会失败。
6. 大 WS 缓冲和 PCM 队列不要放普通 BSS/默认堆，必须显式放 PSRAM。
7. 不要把 `RAW_WS_TEXT_MAX`/`RAW_WS_RX_BUFFER_SIZE` 缩到真实 TTS 消息以下；
   12KB 上限会让连接在第一条正常回复时静默断开（4.1 节根因）。若服务器回复
   超过 768KB base64，两个常量要一起加大。
8. 接收路径在异常/超大帧上关 socket 时必须先 `ws_signal_failure()`，
   否则 `ws_client_wait_done()` 挂死、UI 停在"回复中"。
9. `i2s_play_async()` 拥有缓冲区、失败时自行释放；调用方不要再 free 同一指针。
10. 不要阻塞 `audio_process_task` 发单帧 PCM；PCM 必须拷入队列由 WS 任务发送。
11. 不要在 audio 任务里调用可能阻塞的 WS 停止/销毁（以前导致挂死）。
12. Baidu 3301 是音频质量问题，不要当成 WS 故障。
13. 不要随意改 nginx `proxy_pass` 的尾部斜杠；该 Flask 部署必须
    `proxy_pass http://127.0.0.1:5000;`（无尾斜杠），否则 404。
14. 服务器不要用单 worker + 无超时 + 长 WS 连接：每连接占一线程，死连接会
    耗尽线程池。已改为 32 线程 + 每轮对话后关闭连接 + 空闲 5 分钟超时。
15. 不要抹掉或重置这个 dirty 工作区：里面有大量与本次会话无关的用户改动。
16. 不要在 `afe_lock` 内调用可能长时间阻塞的 `fetch()`（默认超时 2s/实测约
    200ms）：会饿死 feed 任务，表现为"Ringbuffer of AFE is empty"刷屏且唤醒失效。
    用 `fetch_with_delay(短超时)` + feed 信号量驱动（见 4.7）。
17. TTS 播放队列满时不要用超时 0 直接丢音频（会"有回复没声音"）；用有限超时
    阻塞入队 + 队列加大（见 4.9）。
18. LCD 的 待机/聆听/回复 状态只应由 AFE 状态机控制；main.c 不要在 boot/重连
    时强行 `lcd_ui_set_status(LCD_STATUS_LISTENING)`，否则和 `enter_idle()`
    打架，"待机中"显示不出来（见 4.10）。

## 9. 工作区注意事项

仓库刻意保持 dirty，包含大量既有改动（AFE、LCD、I2S、WiFi、main、分区、模型等）。
保留所有无关修改。新的 WS 相关工作只集中在：

- `components/BSP/AI_client/raw_ws_client.c`
- `components/BSP/AI_client/ai_client.c`
- `components/BSP/AI_client/ai_client.h`
- `components/BSP/I2S/i2s.c`
- `components/BSP/AFE_SR/afe_sr.c`
- `main/main.c`
