# MetaOS V2（立创实战派 ESP32-S3）

MetaOS V2 是 `lckfb/szpi-esp32s3` 板子的一个固件变体 `lichuang-dev-metaos`。它保留小智原有的全部功能，并新增：

- TF 卡存储（SDMMC 1 线：CLK=GPIO47，CMD=GPIO48，D0=GPIO21，取自立创官方教程）
- 本地能力：定时提醒、TF 卡音乐播放
- 触屏界面（长按 BOOT 键打开或关闭）

原有的 `lichuang-dev` 变体不受影响。MetaOS 代码只在 `CONFIG_METAOS_ENABLE=y` 时编译。

## 架构

```
用户语音 ──► 小智服务器（ASR + LLM + TTS）
                │  MCP tools/call（设备端工具）
                ▼
ESP32-S3 固件
 ├─ self.reminder.*  提醒 ──► /sdcard/metaos/reminders.json，到点弹窗 + 提示音
 ├─ self.music.*     音乐 ──► /sdcard/music/*.ogg，经通知播放通道输出
 ├─ self.storage.get_info
 └─ [文件管理器]     浏览 TF 卡文件、查看文本（图片查看尚未实现）
```

- 提醒和音乐都在设备上执行。对话的推理仍由小智服务器上的大模型完成，设备本身不运行大模型。
- 所有工具回调都在主任务执行。
- **记忆**：小智服务器原生支持对话记忆，MetaOS 不再另行实现。
- **文件管理器**：本地触屏功能，支持浏览 TF 卡和查看文本文件。

## 构建

先激活 ESP-IDF（推荐 `release/v6.1`），然后：

```sh
python scripts/build.py lckfb/szpi-esp32s3 --name lichuang-dev-metaos --language zh-CN
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
│  └─ reminders.json   待执行的提醒
└─ music/              音乐文件（.ogg）
```

TF 卡在开机时挂载。没有卡或卡无法挂载时，设备照常工作，相关工具会返回"SD 卡不可用"。之后插卡，相关工具调用时会自动重试挂载（每 30 秒最多一次）。不支持运行中热插拔，拔卡前请关机。

## USB U盘模式（TF 卡免拔插拷贝）

编译开关 `CONFIG_METAOS_USB_MSC=y`（默认 / 微信 MetaOS 变体已开启，依赖 `METAOS_ENABLE` + ESP32-S3）。
开启后设备通过原生 USB 口（GPIO19/20）把 TF 卡暴露给电脑当 U 盘，直接读写 `music/`、
`esp_tts_voice_data_xiaole.dat` 等文件，**无需拔卡用读卡器**。基于乐鑫 `esp_tinyusb` 组件（MSC 类）。

### 使用

- **触屏**：设置页 → "USB U盘模式" 开关。
- **语音 / MCP**：`self.usb_msc.set enable=true` 开启，`enable=false` 关闭。

开启后电脑识别到一个 U 盘（默认产品名 "Espressif MSC Device"），可直接拷贝文件。
关闭后卡归还设备，固件自动重载电脑改过的提醒 / 音乐。

### 所有权与安全（重要）

TF 卡的 FAT 文件系统同一时刻只能由一方挂载：

- **设备模式（APP）**：固件挂载 FAT，MetaOS 正常读写。
- **U盘模式（USB）**：固件卸载 FAT，电脑独占。期间设备的音乐 / 提醒 / 文件访问自动暂停，
  避免两边同时写卡损坏 FAT。

进入 U盘模式前，固件会先停止音乐、取消固件安装、等待所有后台文件操作结束（drain），
确认没有打开的文件句柄后才把卡交给电脑——**播放音乐时直接切换也是安全的**。

**务必先在电脑上"安全弹出"再关闭 U盘模式**。固件启用了异步 IO（`CONFIG_TINYUSB_MSC_ASYNC_IO`），
切回设备时会先排空电脑延迟写入的数据再重挂 FAT；但电脑写缓存未 flush 时强行切换仍有风险。

### 限制与注意

- **PHY 互斥**：ESP32-S3 的 USB-OTG 与 USB-Serial-JTAG 共用一个 PHY。U盘模式运行期间
  USB 辅助串口控制台失效；**主 UART 控制台（COM7）不受影响**，烧录仍可用 ROM 下载模式。
- TF 卡仍须 **FAT32**（设备侧）。exFAT 卡设备会提示"需要 FAT32"；电脑侧虽能读 exFAT，
  但设备无法使用，建议统一 FAT32。
- 不支持 U盘模式下热拔卡；拔卡 / 电脑重格式化后若设备无法重挂，到设置页"重新检测"恢复（无需重启）。
- MSC 缓冲 `CONFIG_TINYUSB_MSC_BUFSIZE` 默认 512B，可调大提升吞吐。
- 仅 ESP32-S3 原生 USB 口可用；功能默认关闭，其他板 / 变体不受影响（依赖按 Kconfig 门控，
  不会给非 MetaOS 的 S3 构建引入 TinyUSB）。

## 音乐播放

把 **单声道 Ogg Opus** 文件放进 TF 卡的 `music/` 目录。立创实战派的 codec 是 ES8311（单声道输出）+ ES7210（双声道输入），播放只支持单声道。

在电脑上用 ffmpeg 转换：

```sh
# 推荐的命令（已验证可播放）
ffmpeg -i song.mp3 -vn -ac 1 -ar 48000 -c:a libopus -b:a 32k song.ogg
```

**重要提示**：
- `-vn` 移除视频流（避免包含专辑封面导致播放失败）
- `-ac 1` 强制单声道
- `-ar 48000` 采样率 48kHz（Opus 内部使用）
- 转换后文件约为原 MP3 的 15-20%

- `-ac 1` 强制单声道
- `-ar 24000` 采样率 24 kHz（板子的 codec 默认配置）
- `-b:a 32k` 码率 32 kbps
- `-frame_duration 60` 帧长 60 ms（匹配解码队列大小，减少内存占用）

音乐通过通知播放通道输出（和小智的语音、提示音共享）。播放时：

- 唤醒词、BOOT 按键、提醒到点都会中止播放
- 语音请求的播放会在大模型回复后开始；触屏播放立即开始，除非正在对话
- 支持上一曲/下一曲/停止/随机播放

设备启动时扫描一次音乐库（最多 300 首），按文件名排序。音乐页面每页显示 30 首，可以翻页。

## 触屏界面

**入口**：长按 BOOT 键 1.5 秒，打开或关闭 MetaOS 界面。对话开始时界面自动隐藏。按压说话模式下，长按 BOOT 被占用，无法打开界面。

**显示样式限制**：MetaOS 触屏界面仅在默认样式和微信样式下可用。表情样式会禁用面板（MCP 工具仍然可用）。

### 主屏幕

4 个应用卡片（2x2 网格）：

- 🎵 **音乐**：显示正在播放的歌曲，或歌曲总数
- ⏰ **提醒**：显示最近一条提醒，或"还没有提醒"
- 📁 **文件**：显示当前目录的文件和文件夹数量
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

### 文件页面

完整的文件浏览器，支持：

**文件浏览**：
- 多级目录导航，面包屑路径显示
- 文件夹和文件列表，显示文件大小
- 点击文件夹进入子目录，点击 ".." 返回上级

**图片查看**（JPG/PNG/BMP/GIF）：
- 全屏显示，自动缩放到 320x240
- GIF 动画播放
- 后台异步解码，显示加载动画
- 支持格式：
  - JPEG：硬件/软件解码，自动缩放
  - PNG：流式解码（RGB/灰度），支持透明度
  - GIF：完整动画播放（需 LVGL 内存池空间）
  - BMP：24/32 位未压缩

**文本查看**（TXT/LOG/JSON/MD）：
- 显示文件内容，支持滚动
- UTF-8 编码，最大 64KB
- 只读，不支持编辑

### 设置页面

- **音量**：拖动滑块调整输出音量（松手时应用）
- **亮度**：拖动滑块调整屏幕亮度（松手时应用，最低 5%）
- **TF 卡**：显示已用空间和总容量，或错误信息（如"未检测到 TF 卡"、"TF 卡不是 FAT32 格式"）。点击"重新检测"可以手动触发挂载重试
- **摇一摇切歌**：开启后，摇动设备可以跳到下一首歌（需要 IMU 传感器）
- **翻转停止**：开启后，将设备翻转面朝下可以停止音乐播放或关闭提醒弹窗（需要 IMU 传感器）
- **USB U盘模式**：把 TF 卡暴露给电脑当 U 盘（仅 `CONFIG_METAOS_USB_MSC=y` 时显示，见上文专节）
- **提示**：底部显示"长按 BOOT 键打开或关闭此界面"

### 弹窗提醒

提醒到点时，屏幕中央弹出对话框，显示提醒时间和内容（可以同时显示多条）。按钮：

- **10 分钟后**：小睡 10 分钟，之后再提醒一次
- **知道了**：确认并关闭

弹窗会自动停留 3 分钟，超时后自动关闭。

没有 LVGL 屏幕时（OLED、墨水屏、表情显示），弹窗降级为一行通知。

## 语音播报（暂不可用）

提醒到点时目前只有弹窗和提示音，不会读出提醒内容。

原因：小智协议里设备没有办法让服务器"念一段指定的文字"。唯一能带文字的设备消息是唤醒词检测（`listen` + `state: "detect"`），官方服务器只接受短唤醒词，提醒内容这类长文本会被拒绝（"Detect is for wake words, do not send long texts"）。

后续计划用 ESP-SR 自带的离线中文 TTS 在设备上合成语音，再通过本地播放通道播出，这样不依赖服务器。需要将 `esp_tts_voice_data_xiaole.dat`（约 2.9MB）放到 TF 卡根目录。

## IMU 运动传感器

立创实战派 ESP32-S3 板载 QMI8658 6 轴 IMU（加速度计 + 陀螺仪）。固件会自动探测传感器（I2C 地址 0x6A 或 0x6B）。

**手势检测**（阈值偏保守以防误触，常量在 `main/metaos/metaos_motion.h`，可按硬件微调）：
- **摇一摇切歌**：**摇 2 下 → 下一首，摇 3 下及以上 → 上一首，单下不动作**。判定：加速度幅值 >2.5g 计一次，300ms 不应期避免单次摇晃的多个波峰重复计数；停止摇动 700ms 后按累计次数判定。需在设置中开启。
- **翻转停止**：需**先从正面朝上翻到背面朝下，并连续稳定保持 1.2s** → 停止音乐或关闭提醒弹窗。用滤波后的重力 z + 迟滞带（±600mg）防抖；轻拿轻放、短暂倾斜、或未经翻转直接朝下放置都不会误触；触发后须翻回正面才能再次触发。需在设置中开启。

**MCP 工具**：
- `self.motion.get_state`：查询当前加速度（毫重力单位）、方向（lying_flat）和运动状态（moving）

采样频率 25Hz，后台任务运行，优先级 1。

## MCP 工具列表

设备向小智服务器注册了 9 个 MCP 工具（部分按硬件 / 配置条件注册）：

| 工具 | 功能 |
|------|------|
| `self.reminder.add` | 添加提醒（相对时间或绝对时间，可选每日重复） |
| `self.reminder.list` | 列出所有待执行的提醒 |
| `self.reminder.cancel` | 取消指定 id 的提醒 |
| `self.music.list` | 列出 TF 卡 `music/` 目录下的歌曲（可选关键词过滤） |
| `self.music.play` | 播放歌曲（可选关键词过滤 + 随机播放）。对话在大模型回复后结束，然后音乐开始 |
| `self.music.control` | 控制播放（`action` 为 `"next"` 或 `"stop"`） |
| `self.storage.get_info` | 查询 TF 卡状态、空间、提醒数量、歌曲数量 |
| `self.motion.get_state` | 查询 IMU 运动传感器状态（加速度、方向、运动）—— 仅当 IMU 可用时注册 |
| `self.usb_msc.set` | 开启 / 关闭 USB U盘模式（`enable` 布尔）—— 仅当 `CONFIG_METAOS_USB_MSC=y` 时注册 |

## 已知限制

- SDMMC 只用了 1 线模式（CLK/CMD/D0），读写速度约 3-4 MB/s
- 音乐只支持单声道 Ogg Opus（立体声文件无法播放，这是 OggDemuxer/解码器的限制）
- TF 卡不支持热插拔，拔卡前请关机
- 提醒语音播报需要 ESP-SR TTS 组件和语音数据文件（当前为占位实现）
- MetaOS 触屏界面仅在默认样式和微信样式下可用，表情样式会禁用面板
- 文件管理器：
  - 图片解码在后台线程执行，大图片可能需要几秒加载时间
  - GIF 播放需要 LVGL 内存池有足够空间（约 5× 宽×高 字节）
  - PNG 仅支持非交错模式，不支持调色板
  - 文本文件限制 64KB
  - 当前版本只支持查看，不支持编辑、删除、重命名
- IMU 手势：
  - 摇一摇/翻转检测参数针对典型使用场景调整，可能需要根据实际硬件微调
  - 采样任务始终运行（即使手势功能未启用），功耗约 1-2 mA

## 硬件验证清单

以下功能需要在硬件上测试：

- [ ] TF 卡挂载（SDMMC 1 线，FAT32）
- [ ] 中文长文件名读写
- [ ] 提醒到点弹窗 + 提示音
- [ ] 本地音乐播放（单声道 Ogg Opus）
- [ ] 音乐页面：列表、播放、切歌、随机
- [ ] 提醒页面：列表、删除
- [ ] 文件页面：浏览目录、查看文本
- [ ] 图片查看：JPEG/PNG/BMP 静态图，GIF 动画
- [ ] 图片解码性能（大文件加载时间、内存占用）
- [ ] IMU 传感器探测（QMI8658，I2C 地址 0x6A 或 0x6B）
- [ ] 摇一摇切歌（需调整阈值和时间窗口）
- [ ] 翻转停止音乐/关闭弹窗
- [ ] 设置页面：音量、亮度、TF 卡重检测、手势开关
- [ ] IMU MCP 工具：`self.motion.get_state` 返回正确的加速度和状态
- [ ] USB U盘模式：电脑枚举为 U 盘、读写 TF 卡文件
- [ ] USB 切换安全：播放音乐时开启 U盘模式不崩溃；电脑安全弹出后关闭，文件无损坏
- [ ] USB PHY 互斥：U盘模式下主 UART（COM7）控制台正常、可烧录
- [ ] USB 异常恢复：U盘模式下拔卡 / 电脑重格式化后，"重新检测"能恢复
- [ ] exFAT 卡提示"需要 FAT32"（不误报已挂载）

## Factory 救援变体（lichuang-dev-rescue）

独立的最小救援固件，编译开关 `CONFIG_METAOS_RESCUE_MODE=y`。不依赖 Application/音频/蓝牙/MCP/云，
用于在主固件损坏时从 TF 卡恢复。

- **构建**：`python scripts/build.py lckfb/szpi-esp32s3 --name lichuang-dev-rescue`
- **身份**：BOARD_NAME=`lichuang-dev-rescue`（独立 OTA 身份），BOARD_TYPE=`lichuang-dev`
- **分区表**：`partitions/v2/16m_metaos.csv`（factory@0x20000 + ota_0@0x420000 + ota_1@0x820000 + assets@0xc20000 + coredump@0xfe0000）
- **功能**：LCD+触摸+SD 状态页、TF 卡固件仓库扫描（`/sdcard/metaos/firmware/<channel>/manifest.json`）、
  SHA-256 校验、经 OTA API 安装到 ota_0/ota_1（禁止 factory 与运行中分区）、启动槽切换。
- **UI 语言**：中文。内嵌专用子集字体 `font_rescue_20_4`（20px/4bpp，仅含救援 UI 用到的约 102 个汉字 + ASCII，源自 OFL 许可的 Noto Sans SC）。分区标签 ota_0/ota_1/factory、SHA-256/FAT32、OTA 状态名及底层错误诊断保留英文。改 UI 文案后运行 `python scripts/gen_rescue_font.py` 重新生成 `main/rescue/rescue_font.c`（需 fontTools + node/npx）。
- **签名**：当前仅 SHA-256 完整性校验，发布者签名验证为 Phase 4，UI 如实标注"未验证"。
- **线程模型**：单一常驻工作任务负责所有 SD 挂载/扫描/安装（SdmmcCard 要求 Mount 固定在同一任务）；
  LVGL 操作均在 LVGL 任务内或持 `lvgl_port_lock`；进度经原子变量 + LVGL 定时器传递。

### 分区迁移前提（重要）

新分区表的 factory@0x20000 与旧表 ota_0@0x20000 重叠，**不能通过 OTA 迁移**。
必须串口整表烧录（bootloader + 分区表 + otadata + factory + ota_0 + assets），
保留 0x9000-0xCFFF 的 NVS 区即可保住 WiFi 配置。详见 `METAOS_PHASE1_CHANGE_PLAN.md` §4。
两个 OTA 槽都失效时，bootloader 自动回落到 factory 救援系统。

## 多固件变体与 SD 仓库打包

### 构建变体（config.json 的 builds）

| 变体 | 用途 | 关键配置 |
|---|---|---|
| `lichuang-dev` | 原版小智 | — |
| `lichuang-dev-metaos` | MetaOS 主固件（默认样式） | METAOS_ENABLE、METAOS_USB_MSC、分区表 16m_metaos.csv |
| `lichuang-dev-metaos-wechat` | MetaOS + 微信样式 | 额外 USE_WECHAT_MESSAGE_STYLE（同样含 USB MSC） |
| `lichuang-dev-rescue` | Factory 救援（中文 UI） | METAOS_RESCUE_MODE，无音频/蓝牙/assets |

构建命令（`--language` 由 build.py 控制，会覆盖 config.json；MetaOS 系列用 zh-CN）：

```
python scripts/build.py lckfb/szpi-esp32s3 --name <变体名> --language zh-CN
```

**单 build 目录警告**：build.py 复用同一个 `build/`，切换变体会覆盖 `build/xiaozhi.bin`。
要保留多个变体的产物，编完一个立刻另存（如 `cp build/xiaozhi.bin xxx.bin`）再编下一个。

### 显示样式与 MetaOS 触摸界面的兼容性

- **默认 / 微信样式**：走 SpiLcdDisplay（LVGL + 主题），MetaOS 面板正常，固件管理界面可用。
- **表情动画（emote）**：走 EmoteDisplay，其 `SetTheme()` 是空实现 → 不建立 LVGL 主题 →
  MetaPanel 依赖 `CurrentTheme()` 渲染，拿不到主题就不显示。**这是结构限制，不是配置问题**：
  emote 样式做不出触摸版固件管理界面（MetaOS 语音工具仍可用）。因此未提供 emote 的 MetaOS 变体。

### 救援固件的中文子集字体

救援 UI 用内嵌子集字体 `font_rescue_20_4`（仅含 UI 用到的约 102 个汉字 + ASCII，源自 OFL 的 Noto Sans SC）。
**改过 rescue_ui.cc 的文案后必须重新生成字体**，否则新汉字缺字形：

```
python scripts/gen_rescue_font.py     # 需 fontTools + node/npx（lv_font_conv@1.5.3）
```

脚本从 rescue_ui.cc 的字符串字面量提取非 ASCII 码点，实例化 NotoSansSC 静态字体后生成
`main/rescue/rescue_font.c`。

### SD 固件仓库打包

仓库路径 `/sdcard/metaos/firmware/<名>/manifest.json` + `<bin>`（签名版加 `manifest.json.sig`）。
生成工具 `scripts/firmware_package.py`：

```
# 生成密钥（私钥放仓库外！公钥写进 main/firmware/firmware_pubkey.h）
python scripts/firmware_package.py genkey --out <私密目录> --pubkey-header main/firmware/firmware_pubkey.h

# 打包（只打包 APP 镜像，不要 merged.bin）
python scripts/firmware_package.py package --bin build/xiaozhi.bin \
    --name "默认" --firmware-id xiaozhi-default-metaos --bin-name default.bin \
    --target-board lichuang-dev --channel stable --out "F:/metaos/firmware/default"

# 自验（签名 + bin 哈希）
python scripts/firmware_package.py verify --dir F:/metaos/firmware/default --key <私钥>
```

要点：
- **只打包 app 镜像**（`xiaozhi.bin` / `*.ino.bin`）。merged.bin 的 0x0 是 bootloader、app 在 0x10000，
  塞进 OTA 槽会校验失败；merged 只用于整片烧录。
- `--target-board` 填板型 `lichuang-dev`（安装时与 BOARD_TYPE/BOARD_NAME 比对）。
- 无签名包默认允许安装（`CONFIG_METAOS_FW_REQUIRE_SIGNATURE=n`）；**有签名但验签失败一律拒绝**。
- manifest 的 `signature` 字段只在 `--key` 时写入；无签名字段为空，设备据此区分"未签名/已签名"。
- 默认 / 微信 MetaOS 变体现含 USB MSC。SD 仓库中对应的 MSC 包命名为 `usb-msc` / `wechat-msc`
  （显示名"默认+USB U盘"/"微信+USB U盘"），与早期不含 MSC 的 `default` / `wechat` 包并存，便于回滚对比。

### 切换与安装的可靠方式

- 设备端：MetaOS 或救援的 固件管理 → 启动切换 / 安装（走 `esp_ota_set_boot_partition`，正确维护 otadata）。
- PC 端：优先救援界面或 otatool；**不要手写 otadata 二进制**——entry 的 seq/crc/残留状态容易冲突，
  曾导致 bootloader 选择异常反复回落。otadata 全空白（0xFF）时会启动 factory 救援，可作兜底入口。

### 跨项目固件共存的经验（HID 桥接实验）

- 第三方固件（如 Arduino HID）能否装进 OTA 槽，取决于它**实际需要的分区**是否在共享分区表里。
  HID 固件只用 NVS + coredump，MetaOS 表里都有 → 可以共存；它自带分区表/ bootloader 的 merged 镜像则不行。
- Arduino 固件 `CDCOnBoot=cdc` 会把控制台转到 USB，UART 串口全静默，排查时改用 `CDCOnBoot=default`。
- 第三方固件不自确认 OTA 时，切过去会被 PENDING_VERIFY 回滚；HID 固件自带 ota_confirm 则不会。
