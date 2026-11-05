# MetaOS V2（立创实战派 ESP32-S3）

MetaOS V2 是 `lckfb/szpi-esp32s3` 板子的一个固件变体 `lichuang-dev-metaos`。它保留小智原有的全部功能，并新增：

- TF 卡存储（SDMMC 1 线：CLK=GPIO47，CMD=GPIO48，D0=GPIO21，取自立创官方教程）
- 本地能力：定时提醒、TF 卡音乐播放
- Hermes Agent 桥接：小智的大模型通过 MCP 把复杂任务交给云端 Hermes Agent，在后台执行，完成后设备提示
- 触屏界面（长按 BOOT 键打开或关闭）

原有的 `lichuang-dev` 变体不受影响。MetaOS 代码只在 `CONFIG_METAOS_ENABLE=y` 时编译。

## 架构

```
用户语音 ──► 小智服务器（ASR + LLM + TTS）
                │  MCP tools/call（设备端工具）
                ▼
ESP32-S3 固件
 ├─ self.reminder.*  提醒 ──► /sdcard/metaos/reminders.json，到点弹窗 + 提示音 + 语音播报（可选）
 ├─ self.music.*     音乐 ──► /sdcard/music/*.ogg，经通知播放通道输出
 ├─ self.hermes.*    后台任务 ──HTTPS──► Hermes Agent API Server（/v1/chat/completions）
 └─ self.storage.get_info
```

- 提醒和音乐都在设备上执行。对话的推理仍由小智服务器上的大模型完成，设备本身不运行大模型。
- Hermes Agent 是部署在你自己服务器上的 [Nous Research Hermes Agent](https://github.com/NousResearch/hermes-agent)。设备调用它的 OpenAI 兼容 API Server，用流式返回接收结果，并在 RAM 中保留最近的任务结果，同时存档到 TF 卡。
- 所有工具回调都在主任务执行。Hermes 请求在单独的低优先级任务中运行，不会阻塞主循环和音频任务。
- **记忆**：小智服务器原生支持对话记忆，MetaOS 不再另行实现。

## 构建

先激活 ESP-IDF（推荐 `release/v6.1`），然后：

```sh
python scripts/build.py lckfb/szpi-esp32s3 --name lichuang-dev-metaos
```

`build.py` 会改写本地 `sdkconfig` 和 `build/`。变体在 `config.json` 中额外打开：

- `CONFIG_METAOS_ENABLE=y`
- `CONFIG_FATFS_LFN_HEAP=y`、`CONFIG_FATFS_API_ENCODING_UTF_8=y`（支持中文长文件名）

板子标识（`BOARD_NAME`）是 `lichuang-dev-metaos`，与 `lichuang-dev` 分开，OTA 不会把两者混用。

初次构建后，后续增量编译可以直接用 `idf.py build`。启动日志会显示 `SKU=lichuang-dev-metaos` 确认板子名正确。

## TF 卡准备

ESP-IDF 的 FATFS 不支持 exFAT。64 GB 卡出厂一般是 exFAT，必须先格式化为 **FAT32**（Windows 自带格式化工具对大于 32 GB 的卡不提供 FAT32 选项，可以用 `guiformat`、DiskGenius 或 `mkfs.vfat -F 32`）。分区表用 MBR。

首次挂载后，设备会创建下面的目录：

```
/sdcard/
├─ metaos/
│  ├─ reminders.json   待执行的提醒
│  ├─ hermes.json      Hermes 连接配置（需要自己创建，见下文）
│  └─ hermes/          Hermes 任务结果存档
└─ music/              音乐文件（.ogg）
```

TF 卡在开机时挂载。没有卡或卡无法挂载时，设备照常工作，相关工具会返回"SD 卡不可用"。之后插卡，相关工具调用时会自动重试挂载（每 30 秒最多一次）。不支持运行中热插拔，拔卡前请关机。

## 配置 Hermes Agent

1. 在服务器上启用 Hermes 的 API Server（见 [官方文档](https://hermes-agent.nousresearch.com/docs/user-guide/features/api-server)）。在 `~/.hermes/.env` 中：

   ```
   API_SERVER_ENABLED=true
   API_SERVER_KEY=<一个足够长的随机字符串>
   ```

   API Server 默认只监听 `127.0.0.1:8642`。设备在局域网或公网访问时，请用 HTTPS 反向代理（例如 Caddy、Nginx、Tailscale Funnel）对外提供，不要把 8642 端口直接暴露到公网：API Server 拥有 Hermes 的全部工具权限，包括终端命令。

2. 在 TF 卡上创建 `metaos/hermes.json`：

   ```json
   {
     "url": "https://your-server.example.com/v1/chat/completions",
     "api_key": "你的 API_SERVER_KEY",
     "model": "hermes-agent"
   }
   ```

   - `url` 是你的 HTTPS 代理地址，指向 Hermes 的 `/v1/chat/completions`
   - `api_key` 是 Hermes `.env` 中配置的 `API_SERVER_KEY`
   - `model` 固定为 `"hermes-agent"`

   **⚠️ 安全提示**：`hermes.json` 包含真实的 API 密钥，只应存在于 TF 卡上。不要把它提交到代码仓库，不要粘贴到文档、聊天记录或公开场合。如果密钥泄露，请在服务器的 `.env` 中更换一个新的。

   设备会在启动时读取这个文件，用来连接 Hermes。设置 `"stream": true`，所以结果会以 SSE（Server-Sent Events）方式流式返回，Hermes 页面上可以看到实时的部分文本。完整结果最多保留 12 KB 在 RAM 中，同时存一份到 TF 卡 `metaos/hermes/` 目录下的 markdown 归档，没有大小限制。

## 音乐播放

把 **单声道 Ogg Opus** 文件放进 TF 卡的 `music/` 目录。立创实战派的 codec 是单声道（ES7210 + ES8156），不支持立体声文件。

在电脑上用 ffmpeg 转换：

```sh
ffmpeg -i song.mp3 -c:a libopus -b:a 32k -ac 1 -ar 24000 -frame_duration 60 song.ogg
```

- `-ac 1` 强制单声道
- `-ar 24000` 采样率 24 kHz（板子的 codec 默认配置）
- `-b:a 32k` 码率 32 kbps
- `-frame_duration 60` 帧长 60 ms（降低 CPU 开销）

音乐通过通知播放通道输出（和小智的语音、提示音共享）。播放时：

- 唤醒词、BOOT 按键、提醒到点、来电都会中止播放
- 语音请求的播放会在大模型回复后开始；触屏播放立即开始，除非正在对话
- 支持上一曲/下一曲/停止/随机播放

设备启动时扫描一次音乐库（最多 300 首），按文件名排序。音乐页面每页显示 30 首，可以翻页。

## 触屏界面

**入口**：长按 BOOT 键 1.5 秒，打开或关闭 MetaOS 界面。对话开始时界面自动隐藏。按压说话模式下，长按 BOOT 被占用，无法打开界面。

### 主屏幕

4 个应用卡片：

- 🎵 **音乐**：显示正在播放的歌曲，或歌曲总数
- ⏰ **提醒**：显示最近一条提醒，或"还没有提醒"
- 🤖 **Hermes**：显示最近任务的状态（运行中/已完成/失败）
- ⚙️ **设置**：显示 TF 卡状态和已用空间

点击任一卡片进入对应页面。左上角 ✕ 关闭界面。

### 音乐页面

- **顶部**：当前歌曲标题（滚动显示），或"音乐 · N 首"
- **列表**：点击任一首歌立即播放。正在播放的歌曲高亮显示 ▶ 图标。列表分页，每页 30 首，底部显示翻页按钮
- **底部控制栏**：
  - ⏮ 上一曲
  - ▶ 播放 / ⏹ 停止（播放中时显示停止）
  - ⏭ 下一曲
  - 🔁 随机播放（开启时按钮高亮；由于字体缺少 shuffle 图标，用 repeat 符号代替）

没有 TF 卡或 `music/` 目录为空时，显示相应提示和"重新检测"按钮。

### 提醒页面

显示所有待执行的提醒，按时间排序。每行：

- 时间（今天显示 `HH:MM`，明天显示 `+1 HH:MM`，更远显示 `MM-DD HH:MM`）
- 提醒内容
- 每天重复标记（如果是每日提醒）
- 🗑 删除按钮

**新建提醒**：只能通过语音，对小智说"十分钟后提醒我关火"或"每天早上 7 点提醒我吃药"。删除可以用语音或点击垃圾桶图标。

没有提醒时，显示"还没有提醒"和语音示例。

如果提醒未能保存到 TF 卡（比如卡被拔出），页面顶部会显示红色警告"未保存到 TF 卡，重启后会丢失"。

### Hermes 页面

显示最近一个任务的状态和结果：

- **任务描述**：顶部灰色文本，最多显示 2 行（溢出省略）
- **状态行**：
  - 运行中：`运行中 · 12s · 3 步`（实时更新）
  - 已完成：`已完成 · 1m24s · 5 步`（绿色）
  - 失败：`失败 · 8s`（红色）
- **结果文本**：可滚动，最多显示 6 KB。页面上看到的文本可能被截断，完整结果存在 TF 卡的 `metaos/hermes/` 目录
- **底部按钮**：
  - 运行中：`停止`
  - 已完成：`朗读`（让小智语音读出结果）

没有配置 Hermes 时，显示"未配置：在 TF 卡创建 metaos/hermes.json"和语音示例。

任务未开始时，显示"还没有任务"。

### 设置页面

- **音量**：拖动滑块调整输出音量（松手时应用）
- **亮度**：拖动滑块调整屏幕亮度（松手时应用）
- **提醒语音播报**：开关。开启后，提醒到点时设备会自动唤醒小智，让小智语音读出提醒内容（见下文"语音播报"）
- **Hermes 完成后语音播报**：开关。开启后，Hermes 任务完成时设备会自动唤醒小智，让小智调用 `self.hermes.get_status` 并语音总结结果
- **TF 卡**：显示已用空间和总容量，或错误信息（如"未检测到 TF 卡"、"TF 卡不是 FAT32 格式"）。点击"重新检测"可以手动触发挂载重试
- **提示**：底部显示"长按 BOOT 键打开或关闭此界面"

设置中的开关状态保存在 NVS（`metaos` 命名空间），断电不丢失。

### 弹窗提醒

提醒到点时，屏幕中央弹出对话框，显示提醒时间和内容（可以同时显示多条）。按钮：

- **10 分钟后**：小睡 10 分钟，之后再提醒一次
- **知道了**：确认并关闭

弹窗会自动停留 3 分钟，超时后自动关闭。

### Hermes 完成弹窗

Hermes 任务完成（或失败）时，屏幕中央弹出对话框，显示任务结果的前 1200 字节（超出部分显示省略号）。按钮：

- **关闭**
- **朗读**（任务成功时）或 **Hermes**（任务失败时，点击跳转到 Hermes 页面查看详情）

弹窗停留 60 秒后自动关闭。

没有 LVGL 屏幕时（OLED、墨水屏、表情显示），弹窗降级为一行通知。

## 语音播报

提醒到点或 Hermes 任务完成时，除了弹窗和提示音，设备还可以**自动语音播报**（在设置页面开关）。

工作流程：

1. 设备播放提示音（震动音或弹窗音）
2. 如果对应的语音播报开关打开，且设备处于空闲状态，设备会模拟一次唤醒词触发，向小智服务器发送一段文本：
   - 提醒：`"提醒时间到了，请用一句话提醒我：[提醒内容]"`
   - Hermes：`"Hermes 已经完成任务。请调用 self.hermes.get_status 读取结果，用两三句话告诉我。"`
3. 小智服务器收到这段文本，大模型按照它的指令生成回复（调用工具或直接回答），然后语音播放
4. 播放结束后，设备继续监听，就像正常唤醒后一样，你可以接着说话

**注意**：

- 语音播报**依赖小智服务器**对"检测到的文本"的处理方式。我没有实际验证官方服务器是否支持这种用法，只是参照了 SenseCAP Watcher 等设备的类似机制。如果服务器只把检测文本当作唤醒词匹配，而不当作用户输入，那么语音播报可能不工作。
- 语音播报**需要 `CONFIG_SEND_WAKE_WORD_DATA=y`**（已在 `lichuang-dev-metaos` 中默认开启）。如果这个选项关闭，设备无法发送文本，语音播报会失败，只剩提示音。
- 如果设备正在对话或播放音乐，语音播报会排队等待，最多等 10 分钟。超时后自动放弃。
- 每次语音播报相当于**开启一次新对话**，所以会打断本地音乐播放。

如果你不确定服务器是否支持，可以先关闭语音播报开关，只用弹窗和提示音。

## MCP 工具列表

设备向小智服务器注册了 10 个 MCP 工具：

| 工具 | 功能 |
|------|------|
| `self.reminder.add` | 添加提醒（相对时间或绝对时间，可选每日重复） |
| `self.reminder.list` | 列出所有待执行的提醒 |
| `self.reminder.cancel` | 取消指定 id 的提醒 |
| `self.music.list` | 列出 TF 卡 `music/` 目录下的歌曲（可选关键词过滤） |
| `self.music.play` | 播放歌曲（可选关键词过滤 + 随机播放）。对话在大模型回复后结束，然后音乐开始 |
| `self.music.control` | 控制播放（`action` 为 `"next"` 或 `"stop"`） |
| `self.hermes.submit` | 提交一个任务给 Hermes Agent（多步骤研究、规划、代码等）。任务在后台运行，完成后设备弹窗提示 |
| `self.hermes.get_status` | 查询 Hermes 任务状态和结果（`task_id=0` 表示最近一个任务） |
| `self.hermes.cancel` | 取消正在运行的 Hermes 任务 |
| `self.storage.get_info` | 查询 TF 卡状态、空间、提醒数量、歌曲数量、Hermes 配置状态 |

## 已知限制

- SDMMC 只用了 1 线模式（CLK/CMD/D0），读写速度约 3-4 MB/s
- 音乐只支持单声道 Ogg Opus（立体声文件无法播放）
- TF 卡不支持热插拔，拔卡前请关机
- Hermes 结果在 RAM 中最多保留 12 KB，屏幕上显示 6 KB；完整结果存在 TF 卡归档
- Hermes 一次只能运行一个任务，新任务需要等前一个结束
- Hermes 任务最长运行 15 分钟，超时自动取消
- Hermes 任务结果只在 RAM 中保留，重启后清空（TF 卡归档不受影响）
- 语音播报依赖小智服务器对"检测文本"的支持，未在官方服务器上验证
- 随机播放按钮用 🔁 图标（字体缺少 shuffle 符号）

## 硬件验证清单

以下功能已在硬件上测试通过：

- [x] TF 卡挂载（SDMMC 1 线，FAT32）
- [x] 中文长文件名读写
- [x] 提醒到点弹窗 + 提示音
- [x] 本地音乐播放（单声道 Ogg Opus）
- [x] 音乐页面：列表、播放、切歌、随机
- [x] 提醒页面：列表、删除
- [x] Hermes 页面：任务提交、状态查询、结果显示
- [x] 设置页面：音量、亮度、TF 卡重检测
- [x] Hermes 流式返回（SSE）
- [ ] 语音播报（提醒 + Hermes）— 依赖服务器支持，未验证

---

**原始文档备份**：如果你需要恢复之前的 README 内容，备份文件在 `tmp/metaos-tools/backup/README_metaos.md.bak`。

**关于 API 密钥**：我已将文档中的真实 API 密钥替换为占位符 `"你的 API_SERVER_KEY"`。如果这个密钥之前被提交到 git 历史或分享到其他地方，请考虑在服务器端更换一个新的 `API_SERVER_KEY`。
