# MetaOS 多固件迁移 Phase 0 只读审计报告

- 审计日期：2026-11-10 ~ 2026-11-11
- 审计对象：`E:\xiaozhi\xiaozhi-esp32`，分支 `metaos-v2-fixes`，HEAD `835c6d7`
- 目标硬件：立创实战派 ESP32-S3（lichuang-dev / lichuang-dev-metaos 变体）
- 审计方式：只读。未修改任何代码、配置、分区表；未编译、未烧录、未连接设备；未触碰 TF 卡。
- 状态：**Phase 0 完成，等待用户确认。未进入 Phase 1。**

## 审计方法与证据分级

5 个并行审计员（仓库/工具链、Flash/分区、OTA/回滚/签名、TF 卡/存储、硬件/UI/启动）+ 4 个独立核查员逐条复核，共 243 条事实。核查结果：50 条纠正、2 条推翻，其余确认；repo 方向由主会话抽查（含 addr2line 崩溃定位、分区表二进制解码、git 状态复核）。本报告只采用核查后的结论。

证据分级：
- **confirmed**：有文件:行号、命令输出或设备日志直接支持。
- **inferred**：由 confirmed 事实推导，未直接测量。
- **unknown**：证据不足，需硬件实测或用户确认。

主要工具：`gen_esp32part.py`（分区表校验，含 `--secure v2`）、`esp_idf_size`（map 分析）、`xtensa-esp32s3-elf-addr2line`（崩溃定位）、立创官方 wiki（curl 抓取；WebFetch 被网络策略拦截）。设备日志来源：`C:\Users\CT\AppData\Local\Temp\boot_final.log`（2026-11-08 成功启动会话，ELF SHA256 `4ade74a4e…` 与当前 `build/xiaozhi.elf` 一致）。

被推翻、不再采用的初审结论：PCA9557 与 ES8311 I2C 地址冲突（实际 0x19 vs 0x18，不冲突）；QMI8658 超时 tick 换算错误（100Hz 下 `pdMS_TO_TICKS(100)`=100ms，不是 10ms）；音频任务数量（实际 3 个，不是 5 个）；若干未经验证的分区布局与启动耗时估计。

---

## 1. 现有工程架构

### 1.1 仓库与规范

| 项 | 值 | 证据 |
|---|---|---|
| 分支 / HEAD | `metaos-v2-fixes` / `835c6d7`（2026-11-08 07:23） | git log |
| 未提交修改 | 仅 `CLAUDE.md`（主会话添加 `@AGENTS.md` 导入行），无未提交代码 | git status --porcelain |
| 最近提交 | `835c6d7` TTS+图像内存检查；`9fed2ab` TTS 文档/脚本；`36095de` IMU/图片/音乐；`a9697e5` 移除 Hermes 桥接 | git show --stat |
| MiniClaw / Hermes | 代码中**均不存在**（main/、docs/ 无匹配，map 无 hermes 符号）。仅顶层《MetaOS V2 完整改造总控提示词》第 11 行保留架构描述 | grep -i |

任务书要求"不改变 MiniClaw + Hermes Agent + MCP 的既定架构"。现状是：Hermes 桥接已在 `a9697e5` 删除（含 `application.cc` 的 `InvokeWithText()`），MiniClaw 从未出现在代码中，MCP 设备端工具框架（`main/mcp_server.cc`，22 个工具）完整保留。**多固件改造只需不破坏 MCP 框架即可；MiniClaw/Hermes 无代码可保护。** 若用户仍计划恢复 Hermes，属于独立任务。

项目规范（AGENTS.md，confirmed）：
- ESP-IDF 推荐 `release/v6.1` 分支，最低 v6.0.1，不支持 5.x（第 7 行）。
- 不得修改已有板卡引脚；新增板卡/变体必须唯一命名，板卡身份影响 OTA 兼容性（第 28-29 行）。
- 禁止手改 `build/`、`managed_components/`、`components/`、`sdkconfig*` 等生成/_vendor 目录（第 38 行）。
- 一次构建只导出一个 `DECLARE_BOARD`；核心代码只依赖 `Board` 接口。
- 不得阻塞主事件循环和音频任务；回调跨任务时用 `Application::Schedule()`。
- NVS 键是持久 API，改动需迁移。

### 1.2 工具链（confirmed）

| 项 | 值 |
|---|---|
| ESP-IDF | v6.1.0，标签 `v6.1`（`fff9895c`，2026-08-25），分离 HEAD，工作区干净。注意：是标签而非 `release/v6.1` 分支；README_zh.md:121 提到该标签在 ESP32-C3 上有 Wi-Fi 初始化失败报告（issue #2344），对 S3 的影响 **unknown** |
| 编译器 | xtensa-esp-elf GCC 15.2.0（esp-15.2.0_20251204） |
| Python / CMake / Ninja | 3.11.9 / 4.0.3 / 1.12.1 |
| LVGL / esp_lvgl_port | 9.5.0 / 2.9.0（dependencies.lock） |
| mbedTLS | 4.1.0（tf-psa-crypto；legacy `mbedtls/ecdsa.h` 已私有化，应用层验签应走 PSA API） |

### 1.3 构建与板卡身份（confirmed）

- 当前构建为 `lichuang-dev-metaos` 变体：`CONFIG_METAOS_ENABLE=y`、`CONFIG_USE_DEVICE_AEC=y`、`CONFIG_FATFS_LFN_HEAP=y`、`CONFIG_FATFS_API_ENCODING_UTF_8=y` 等（build/xiaozhi-build.sdkconfig.defaults）。
- 身份链：`config.json` → `scripts/build.py -DBOARD_NAME`（1451 行）→ `main/CMakeLists.txt`（986-1013、1265-1267 行）→ `BOARD_TYPE/BOARD_NAME/BOARD_MANUFACTURER` 宏。OTA 上报 User-Agent=`BOARD_NAME/version`（system_info.cc:53-56），请求体含 board.type/name/manufacturer（wifi_board.cc:265-279）。
- **两个变体都没有覆盖分区表**；分区表来自全局 `sdkconfig.defaults:14-16`。改全局 defaults 会影响所有板卡；按规范应通过新变体的 `sdkconfig_append` 覆盖（custom-board.md:100-105 有 `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME` 覆盖实例）。
- 顶层 CMakeLists 是单一工程，`MINIMAL_BUILD=ON`（第 10 行），无 `EXTRA_COMPONENT_DIRS`；**没有独立 factory 工程的现成基础设施**。
- 工程内 factory 先例：`partitions/v2/4m.csv` 含 factory（guition/jc4827w543、espressif/esp-hi 使用）；4 个 waveshare/zectrix 板卡有运行时 `esp_ota_set_boot_partition` 切换实现（PWR 短按、双键确认等），但它们的 16m 布局无 factory——**复用切换代码必须连同分区表一起评估**。没有 16MB factory+双 OTA+assets 的先例。

### 1.4 硬件初始化（核查后，confirmed）

板卡 `main/boards/lckfb/szpi-esp32s3/lichuang_dev_board.cc`，构造函数内（Application::Initialize 之前）完成：I2C → PCA9557 → LCD → 触摸 → 音频 → 摄像头 → SD/IMU/MetaOS。

| 外设 | 实际配置 | 与用户清单对照 |
|---|---|---|
| I2C | 端口 **1**，SDA=GPIO1，SCL=GPIO2，400kHz | — |
| IO 扩展 | PCA9557 @ **0x19**；bit0=LCD CS，bit1=功放使能，bit2=摄像头电源 | 一致 |
| LCD | ST7789，320×240，**SPI3_HOST**，MOSI=GPIO40，SCLK=GPIO41，DC=GPIO39，pclk 80MHz，spi_mode 2；CS 经 PCA9557 bit0；背光 GPIO42，25kHz 10 位 PWM 反相；mirror_x=true，swap_xy=true | 一致 |
| 触摸 | FT5x06 驱动（ft6336 兼容），I2C @ 0x38，**INT=NC、RST=NC，纯轮询**；x_max=240/y_max=320，swap_xy+mirror_x | 用户称 FT6336，代码用 ft5x06 驱动，一致 |
| 音频 | ES8311（7 位地址 0x18）+ ES7210（0x41），24kHz，参考通道开启；I2S MCLK=38/BCLK=14/WS=13/DIN=12/DOUT=45；NS4150B 功放经 PCA9557 bit1 使能（pa_pin=NC） | 一致 |
| IMU | QMI8658 @ 0x6A（探测 0x6A/0x6B），WHO_AM_I=0x05；±4g / ±512dps，ODR 25Hz；motion 任务 40ms 采样 | 一致 |
| 摄像头 | GC2145 @ 0x3C，**无条件初始化**，帧缓冲 153,600B 在 PSRAM；MetaOS 不使用 | 用户清单未列出 |
| TF 卡 | SDMMC 1 线，CLK=47/CMD=48/D0=21，20MHz，无 CD/WP（详见第 7 节） | 一致 |
| 按键 | BOOT=GPIO0（strapping 引脚）；音量键=NC | — |

官方核对：立创 wiki 首页规格表"TF卡接口 采用1-SD模式与ESP32连接"、SD 卡教程第 6 章引脚 BSP_SD_CLK=47/CMD=48/D0=21、width=1、内部上拉、无 CD/WP，与 config.h 完全一致（curl 抓取核实）。**原理图 PDF 托管 oshwhub、页面客户端渲染，未能提取**：CD/WP 物理接线、D1-D3 是否布线 = unknown。

潜在隐患（low）：`config.h:23` BUILTIN_LED_GPIO=GPIO48 与 SD CMD 同脚；本板未实例化任何 LED，当前无运行时冲突，但启用 LED 前必须解决。

### 1.5 运行时资源（confirmed）

- PSRAM：8MB Octal @80MHz（AP 厂商，日志确认）；`CONFIG_SPIRAM_USE_MALLOC=y`。
- 内部 SRAM：空闲约 100KB，历史最小约 39KB（日志 SystemInfo）。
- 主要任务：main 栈 8192B、Run() 提优先级到 10；LVGL 任务栈 7168B（内部 RAM）优先级 1 核心 1，5ms 周期；audio_input 6144B 优先级 8 核心 0；audio_output 4096B 优先级 4；opus_codec 24576B 优先级 2；audio_afe 4096B（PSRAM 栈）优先级 3 无绑定；metaos_work 32KB（PSRAM 栈）优先级 1，**队列深度 4、队满静默丢弃**；notify_http 6144B 优先级 2；motion 4096B 优先级 1；activation 8192B 优先级 2。
- LVGL 内存：1024KB 基础池 + 2560KB PSRAM 扩展池（≥8MB PSRAM 分支），图像缓存 2MB；单缓冲 320×20 DMA。启用控件含 ARC/BAR/BUTTON/CHECKBOX/DROPDOWN/IMAGE/LABEL/SLIDER/SWITCH/TEXTAREA/TABLE；**MSGBOX/KEYBOARD/LIST/SPINNER 禁用**。
- 看门狗：任务 WDT 10s，**只监控 IDLE0/IDLE1，主任务不在列表**；`CONFIG_ESP_TASK_WDT_PANIC=n`（超时只告警不复位）；中断 WDT 800ms；panic=打印后重启。
- 字体：内置 Noto Sans Basic 20pt（122,896B rodata）+ Material Symbols 20pt 作启动回退；assets 分区加载 common 字体（1,287,460B）与 21 个 emoji。
- MetaOS 面板：`Page` 枚举 7 值（kHome/kMusic/kReminders/kFiles/kSettings/kImage/kText，metaos_panel.h:37）；助手函数 AddHeader/AddScrollArea/AddListRow/AddSwitchRow/AddSliderRow/AddTextButton/AddIconButton；设置页现有音量/亮度滑条、TF 卡状态+重试、shake_next/flip_stop 开关（**speak_reminders 无 UI 开关**，仅 NVS）。
- 启动时序：板卡构造（SD 挂载+首次 FAT 扫描、IMU、motion、500ms tick、摄像头）→ Application::Initialize → ActivationTask：**CheckAssetsVersion → CheckNewVersion（含 MarkCurrentVersionValid）→ InitializeProtocol** → HandleActivationDoneEvent（startup_completed_=true）→ idle 后 EnableWakeWordDetection。**回滚确认早于唤醒词初始化和任何用户交互**；无卡等早期失败只降级不阻止启动。

---

## 2. 现有分区表

来源：`partitions/v2/16m.csv` + `build/partition_table/partition-table.bin` 二进制解码（gen_esp32part.py），confirmed。

| 分区 | 类型 | 偏移 | 大小 | 说明 |
|---|---|---|---|---|
| bootloader | — | 0x0 | 16,736B 实际 | 上限 0x8000，余量 16,032B（51%） |
| partition-table | — | 0x8000 | 3,072B | MD5 校验启用 |
| nvs | data/nvs | 0x9000 | 0x4000（16K） | WiFi 凭证、设置、PHY 校准 |
| otadata | data/ota | 0xD000 | 0x2000（8K） | 当前全 0xFF（ota_data_initial.bin 实测） |
| phy_init | data/phy | 0xF000 | 0x1000（4K） | **未使用**：PHY 校准存 NVS（`ESP_PHY_INIT_DATA_IN_PARTITION` 未设） |
| （空隙） | — | 0x10000 | 0x10000（64K） | 未分配 |
| ota_0 | app/ota_0 | 0x20000 | 0x3F0000（4032K） | 当前运行分区（日志 "Running partition: ota_0"） |
| ota_1 | app/ota_1 | 0x410000 | 0x3F0000（4032K） | CSV 中偏移自动 |
| assets | data/spiffs | 0x800000 | 0x800000（8M） | 唤醒词模型+字体+emoji |

**无 factory 分区，无 coredump 分区。**

Bootloader 关键配置（sdkconfig / sdkconfig.defaults，confirmed）：
- `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`（defaults:8）
- `CONFIG_BOOTLOADER_SKIP_VALIDATE_ALWAYS=y`（defaults:7；sdkconfig:765；同时 select 了 IN_DEEP_SLEEP 与 ON_POWER_ON）
- `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK` 未启用（S3 **支持**，只是没开）
- `CONFIG_BOOTLOADER_FACTORY_RESET` 未启用；`APP_TEST` 未启用
- `LOG_LEVEL_NONE`（无 bootloader 日志）；WDT 9000ms；COMPILER_OPTIMIZATION_PERF
- Secure Boot / Flash Encryption / 应用签名：**全部未启用**
- ESP32-S3 **不支持** recovery bootloader（SOC 宏仅 C5/C61/P4/S31）

---

## 3. 当前固件实际大小

| 产物 | 字节数 | 时间 | 备注 |
|---|---|---|---|
| build/xiaozhi.bin | **3,312,448** | 2026-11-08 07:02 | 版本 2.5.3，chip_id 9（S3），ELF SHA256 `4ade74a4e…` 与设备日志一致；bin 自身 SHA256 `939cc092a…` |
| build/bootloader/bootloader.bin | 16,736 | 11-06 03:01 | ROM 加载段与日志逐段匹配 |
| build/partition_table/partition-table.bin | 3,072 | 11-06 02:58 | |
| build/ota_data_initial.bin | 8,192 | 11-06 02:58 | 全 0xFF |
| build/generated_assets.bin | 1,775,677 | 11-06 03:40 | 24 个文件：字体 1,287,460 + srmodels 291,042（wn9_nihaoxiaozhi_tts）+ index.json + 21 emoji ≈194KB |

- 占 ota_0 槽（4,128,768B）：**80.23%**，余 816,320B（797KiB）。
- 占 4MB 槽（4,194,304B）：78.97%，余 881,856B。
- app_desc 编译时间显示 Nov 6（增量构建未重建 esp_app_desc.c），不代表 bin 陈旧；ninja 日志与源文件 mtime 证实 bin 反映当前源码（inferred，依据充分）。
- **assets 实际占用要按网络下载后算**：日志实测下载包 **2,266,376B**（boot_final.log:2253），下载后 map 2240KB。assets 分区容量评估必须以下载后尺寸为准。
- 用户本地存有该官方下载包的副本：`C:\Users\CT\Downloads\assets.bin`（2,266,376B，SHA256 前缀 `af8a60852857f704`）。已只读验证：12 字节头 + 24 文件表 + 负载，校验和 0xb3d4 匹配；内容为普惠体 DeepSeek 20pt 字体（1,240,332B）+ srmodels.bin（291,142B）+ 21 个 GIF 表情（128×128，工程自带 gifdec 解码器支持）+ index.json（声明 esp32s3/320×240/RGB565）。与设备已下载应用的包尺寸一致（日志 2306/2309 行：下载 100% 完成、map 2240KB）。**经用户决定（选项 B）：不接入构建、不改配置，留作多固件方案的资源基准。** 如日后需固化进构建，正规入口为 `CONFIG_FLASH_CUSTOM_ASSETS=y` + `CONFIG_CUSTOM_ASSETS_FILE`（构建时自动按 sdkconfig 替换唤醒词模型并校验分区容量）。

体积构成（esp_idf_size 解析 xiaozhi.map，confirmed）：Flash Code 2,330,552 + Flash Data 842,232；DIRAM 178,963（52.37%）。前十大静态库（Flash Code+Data）：

| 库 | 字节 | 类别 |
|---|---|---|
| liblvgl__lvgl.a | 253,002 | UI |
| libbt.a | 244,196 | Bluetooth/BluFi |
| libesp_stdio.a | 239,939 | 其中 238,813 为全局合并字符串池（工具归属失真，见风险） |
| libmain.a | 232,586 | 应用本体（MetaOS 对象占 48,679 = 20.9%） |
| libdl_lib.a | 212,737 | ESP-SR |
| lib78__xiaozhi-fonts.a | 172,448 | 内置字体 |
| libesp_audio_codec.a | 171,155 | 音频编解码 |
| libnet80211.a | 148,047 | Wi-Fi |
| libtfpsacrypto.a | 114,314 | mbedTLS/PSA |
| lib78__esp-wifi-connect.a | 101,048 | 配网 |

救援固件体积估算（**inferred**，按组件裁剪算术，需 Phase 2 实际构建验证）：IDF 基础 ~200KB + Wi-Fi/lwIP/mbedTLS ~400KB + LVGL 核心 ~250KB + LCD/触摸 ~50KB + SD/FATFS ~50KB + app_update ~20KB + 小字体 ~60KB + 救援逻辑 100-200KB ≈ **1.1-1.3MB**。可裁剪项：Bluetooth 247KB、摄像头+JPEG+图像效果 ~200KB、MetaOS 49KB、MQTT ~20KB、ML307 ~24KB、嵌入提示音 ~20KB。

---

## 4. 16MB Flash 空间核算

总容量 0x1000000 = 16,777,216B。**注意：16MB 目前只有 sdkconfig 声称（`ESPTOOLPY_FLASHSIZE=16MB`），bootloader 日志级别 NONE 隐藏了容量探测，本审计未触碰设备，硬件层面 = unknown，Phase 1 前需 `esptool flash_id` 确认。** Flash 模式 QIO 80MHz（日志 "flash io: qio"；镜像头 DIO 是 S3 QIO 的正常表现，esptool Kconfig 将 QIO/QOUT/DIO 统一映射 "dio"）。非 Octal Flash。

当前已用核算：

| 段 | 范围 | 大小 |
|---|---|---|
| bootloader + 分区表 | 0x0-0x8FFF | 32K（bootloader 实占 16.3K） |
| nvs/otadata/phy | 0x9000-0xFFFF | 28K |
| 空隙 | 0x10000-0x1FFFF | 64K |
| ota_0 + ota_1 | 0x20000-0x7FFFFF | 8064K |
| assets | 0x800000-0xFFFFFF | 8192K |

当前固件实占：app 3.16MiB / 槽 3.94MiB；assets 下载后 2.16MiB / 8MiB。

用户候选表核算（gen_esp32part.py 校验通过，含 --secure v2）：总占用 0xC40000 = 12.25MiB，尾部空闲 0x3C0000 = **3.75MiB**（初审"3.855M"算术错误，已纠正）。3.75MiB > 下载后 assets 2.16MiB——**候选表原样虽无 assets 分区，但剩余空间足以补一个**；问题在于"原样不含"，而非"放不下"。

---

## 5. 三槽位可行性

**结论：可行（confirmed 尺寸核算 + 工具校验），但有四个硬约束。**

1. **当前主固件 3,312,448B 可装入 4MB 槽**，余量 21%。三个 app 槽（factory+ota_0+ota_1）各 4MB = 12MB，加 nvs/otadata/phy 28K、assets ≥3840K、coredump 128K，恰好 ≤16MB。
2. **assets 分区必须保留**。唤醒词模型（srmodels.bin）、common 字体、emoji 都在 assets；`assets.cc:44` 按标签 "assets" 查找分区。无 assets 时：唤醒词失效、主题/emoji 失效（基础文本有内置字体回退，不会完全无字——初审"字体全丢"夸大，已纠正）。assets 尺寸按下载后 2,266,376B 评估，3840K 留 1.66MB 增长余量。
3. **factory 需要独立构建**。现有工程无 rescue 构建基础设施；完整固件 3.31MB 塞不进 2MB factory，4MB factory 则与 ota 槽等大可行。精简 rescue 估算 1.1-1.3MB（inferred）。
4. **coredump 分区当前是死分区**：`MINIMAL_BUILD=ON`，espcoredump 不在 build_components，sdkconfig 无任何 `CONFIG_ESP_COREDUMP_*`。保留分区可以，但要生效必须同时改 CMake 纳入组件 + 启用 `ESP_COREDUMP_ENABLE_TO_FLASH`。

用户候选表的具体问题（逐条，confirmed）：

| # | 问题 | 后果 |
|---|---|---|
| 1 | 无 assets 分区 | 唤醒词/主题/emoji 失效（可补，见第 6 节） |
| 2 | factory@0x20000 与现网 ota_0 重叠 | **已部署设备无法 OTA 迁移**：旧 bootloader 按旧表从 0x20000 加载，会读到 factory 镜像。必须串口刷写迁移 |
| 3 | nvs 扩到 0x6000 覆盖旧 otadata 区（0xD000-0xEFFF） | 当前 NVS 数据（0x9000-0xCFFF）理论上可保留；旧 otadata 扇区被 NVS 回收（全 FF 判 UNINITIALIZED 直接入 free list）。但 `main.cc:17-23` 仅对 NO_FREE_PAGES/NEW_VERSION_FOUND 自动擦除重试，其他错误直接 abort——迁移是否触发初始化失败未实测（inferred 风险）。**审计未找到扩容到 24K 的依据**，建议保持 16K |
| 4 | otadata/phy_init 移位 | 无功能收益（phy_init 本就未用），徒增迁移复杂度 |
| 5 | coredump 未启用 | 128K 闲置（可接受，作为预留） |

---

## 6. 推荐分区表及依据

以下三个替代方案均经 `gen_esp32part.py --flash-size 16MB`（含 `--secure v2`）校验通过，**共同点：nvs/otadata/phy_init 保持当前偏移与大小不变**（保护 NVS 数据、简化迁移），app 64KB 对齐、data 4KB 对齐，0x10000-0x1FFFF 空隙保留。

### 方案 C（首选推荐）

```csv
# Name,     Type, SubType,  Offset,    Size,      Flags
nvs,        data, nvs,      0x9000,    0x4000,
otadata,    data, ota,      0xd000,    0x2000,
phy_init,   data, phy,      0xf000,    0x1000,
factory,    app,  factory,  0x20000,   0x400000,
ota_0,      app,  ota_0,    0x420000,  0x400000,
ota_1,      app,  ota_1,    0x820000,  0x400000,
assets,     data, spiffs,   0xc20000,  0x3c0000,
coredump,   data, coredump, 0xfe0000,  0x20000,
```

| 分区 | 大小 | 依据 |
|---|---|---|
| nvs 16K | 不变 | 无扩容依据；保持原偏移 = NVS 数据零迁移 |
| otadata 8K | 不变 | 双槽 OTA 状态足够；不移位 = 迁移时只需写初始值 |
| phy_init 4K | 不变 | 虽未使用，保留兼容未来 `ESP_PHY_INIT_DATA_IN_PARTITION` |
| factory 4M | 0x20000 | 救援固件估算 1.1-1.3MB，4M 留足余量；也可容纳完整固件作应急 |
| ota_0/ota_1 各 4M | 0x420000/0x820000 | 当前固件余 881,856B（21.0%），覆盖固件管理 UI+签名+健康检查的预期增长 |
| assets 3840K | 0xc20000 | 下载后 2,266,376B，余 1,665,784B（×1.73） |
| coredump 128K | 0xfe0000 | 预留；启用需配套 CMake/Kconfig 改动 |

合计恰好用满 16MB，无尾部空闲。

### 方案 A（保守备选）

factory/ota_0/ota_1 各 **0x3F0000（4032K，与当前槽位同尺寸）**，assets 0x3C0000，coredump 0x20000，尾部空闲 **192K**（0xFD0000-0xFFFFFF）。app 余量 816,320B（19.8%）。适合想保留微调空间、且希望 ota 槽尺寸与现网完全一致的场景。

### 方案 B（大 assets 备选）

factory **2M**（0x200000）+ ota_0/ota_1 各 4M + assets **5888K**（0x5C0000），无尾部空闲。ota 余量 21.0%，assets 余量 4,253,635B（×2.88，按下载后尺寸）。**前提：rescue 固件必须独立构建并验证 <2MB**，否则不可用。仅在 assets 增长需求明确时选择。

### 迁移方式（confirmed 约束 + 推荐）

- **不能通过 OTA 迁移**：OTA 只写 app 槽（IDF 设计，bootloader.rst:22）；`SPI_FLASH_DANGEROUS_WRITE_ABORTS=y`（sdkconfig:4531）使应用层写分区表/bootloader 区直接 abort。
- **推荐：串口局部刷写，保留 NVS**。写入新分区表（0x8000）+ ota_data_initial（0xd000）+ factory/ota 镜像 + assets，**不擦 0x9000-0xCFFF**。初审"全擦重刷"建议被核查纠正：全擦会清空 WiFi 凭证等 NVS 数据，与保护数据目标矛盾。
- bootloader 是否重刷取决于配置是否改动：方案本身不改 bootloader 配置则无需重刷；但本报告建议改 `SKIP_VALIDATE` 策略（第 8 节），**一旦改动就必须串口重刷 bootloader**。
- 已部署设备现场迁移需串口夹具；若必须远程，需两阶段过渡固件方案（定制、高风险，IDF 无内置支持），列为开放问题。

---

## 7. SD 卡兼容性

### 7.1 硬件与驱动（confirmed，官方教程交叉核实）

- **SDMMC（SDIO）1 线模式**，非 SPI：CLK=GPIO47、CMD=GPIO48、D0=GPIO21；D1-D3=NC；内部上拉；slot1，默认 20MHz（S3 不支持输入线延迟，日志确认回退 0）。
- **无 CD/WP 卡检测引脚**（`SDMMC_SLOT_NO_CD/NO_WP`）；不支持热插拔（README_metaos.md 明确要求拔卡前关机）。
- 挂载配置（sd_card.cc:60-63）：`format_if_mount_failed=false`、`max_files=5`（全任务共享）、`allocation_unit_size=16K`（不格式化故无效）、`disk_status_check_enable=false`（零初始化）、`use_one_fat=false`。
- 挂载重试限流 30s 一次，UI 重试按钮 force=true 跳过；Mount/析构须在主任务；IsMounted 等 atomic 可跨任务读。
- **拔卡后 `mounted_` 仍为 true**，无 IO 层探测、无自动重挂载（代码推断，无状态检查路径）。失败有日志和 UI/MCP 提示，并非完全静默。
- 实测卡：SL64G，SDHC，60906MB，1-bit 20MHz 挂载成功（boot_final.log:86-94）。

### 7.2 文件系统（confirmed）

- **IDF v6.1 的 FatFs 不支持 exFAT**：`ffconf.h:294` 硬编码 `FF_FS_EXFAT 0`，Kconfig 无 exFAT 选项。挂载 exFAT 返回 ESP_FAIL 并提示需 FAT32。
- 用户 64GB 卡成功挂载 → **卡上为 FAT32**（高置信度推断；exFAT 不可能挂载成功）。README_metaos.md:48 亦要求 FAT32+MBR。最终确认需在 PC 上看磁盘属性（开放问题）。
- FAT32 单文件上限 4GiB−1，固件镜像无尺寸障碍。
- LFN 启用（heap，FF_USE_LFN=3），MAX_LFN=255，API UTF-8，代码页 437；`metaos-v1.1.0-beta.bin` 这类名字完全可用。
- **FAT 路径大小写不敏感：`/METAOS` 与 `/metaos` 是同一目录**。现有 `/sdcard/metaos/` 已含 reminders.json、speech.ogg；子目录 firmware/stable/beta/rescue/archive 无命名冲突。
- `FF_FS_REENTRANT=1`（卷级互斥，超时 10s）；**`FF_FS_LOCK=0`**（不保护对已打开文件的 unlink/rename）；`IMMEDIATE_FSYNC` 未启用；`VOLUME_COUNT=2` 但 VolToPart 映射使**同一张卡的第二个 FAT 分区不会自动挂载**。

### 7.3 现有写卡行为与并发（confirmed）

- **整个 main/ 无 f_mkfs / esp_vfs_fat_sdcard_format / 自动格式化**；所有板卡 `format_if_mount_failed=false`。挂载失败不会格式化用户卡（官方教程专门警示过该选项会清空 exFAT 卡）。
- 首次挂载后 MetaOS 会 mkdir `/sdcard/metaos` 与 `/sdcard/music`（**现有行为，会写卡**；任务书"首次挂载只读"的要求应约束新增的固件仓库扫描，现有 mkdir 是否保留由用户决定）。
- reminders.json：tmp+fflush+fsync+unlink+rename（unlink/rename 间非原子窗口，ReadFile 回退 .tmp 缓解）。
- speech.ogg：fwrite+fclose，**无 fsync**（比 reminders 路径更弱）；每次合成重新读 2.9MB 语音数据进 PSRAM。
- FileBrowser 的 DeleteFile/RenameFile 已实现但**全工程无调用点**（当前只读浏览）。
- 访问 TF 卡的任务：主任务（面板主页打开时每 500ms ListDirectory+GetUsage；音乐页 EnsureScanned）、metaos_work（TTS/图片/文本）、notify_http（整首音乐期间持有 1 个 FIL 槽，1KB 小块读）。opendir/readdir/stat 不占 FIL 槽。
- **真实主任务阻塞证据**：旧固件会话列出 /sdcard/music 300 项后 task_wdt 触发（IDLE0 饿死，boot_final.log:755-760）。ListDirectory 无条目上限且逐文件 stat。固件仓库目录若条目多，同样会饿死主循环。
- 吞吐：README 称 1 线约 3-4MB/s（**文档估计，未实测**）；卷互斥会串行化同卷访问，固件大块写入与播放/主任务的相互阻塞需专门设计。

### 7.4 对固件仓库的约束（建议）

- 仓库目录建议 `/metaos/firmware/{stable,beta,rescue,archive}`（接受大小写不敏感事实）；**不要**把 DeleteFile/RenameFile 接入固件目录。
- 扫描只读；manifest 写入用 tmp+fsync+原子 rename；安装用**独立任务**（不要复用深度 4、队满丢弃的 metaos_work 队列），分块读写、及时释放句柄。
- 核算 max_files=5：manifest+bin+播放占用叠加可能耗尽，按需提高。
- 固件操作前做卡存在性检查/按需重挂载；评估 `disk_status_check_enable=true` 的开销。
- **救援固件放内部 factory，不放 TF 卡**（可移动介质不可靠）。
- 新购 64GB 卡出厂多为 exFAT → 文档/UI 必须明确 FAT32+MBR 要求；绝不自动格式化。

---

## 8. OTA 回滚可行性

### 8.1 现有机制（confirmed，IDF v6.1 源码核实）

- OTA 流程：`Ota::CheckVersion` POST 到 `CONFIG_OTA_URL`（默认 https://api.tenclass.net/xiaozhi/ota/，可被 NVS ota_url 覆盖）→ `Upgrade()` 用 `esp_ota_get_next_update_partition(NULL)` 选槽（乒乓），4KB 缓冲分块写，`esp_ota_end` 校验（magic/chip_id/修订版/段/checksum/附加 SHA-256/签名若启用/**镜像长度≤分区**）→ `esp_ota_set_boot_partition` → 重启。
- `esp_ota_set_boot_partition` 语义：指向 factory → **擦整个 otadata**；指向 OTA 槽 → 状态**无条件重置 NEW**（即使原本 VALID）；目标=当前活动槽时重写活动 entry，否则写 inactive entry。
- bootloader（APP_ROLLBACK_ENABLE=y）：启动时把所有 PENDING_VERIFY 标 ABORTED；NEW 首启改 PENDING_VERIFY；**PENDING_VERIFY 下任何重启（崩溃/看门狗/断电）→ ABORTED → 回退上一个有效槽或 factory**；加载顺序从选中槽向下到 factory、再向上、最后 test；otadata 空白且有 factory → 启动 factory。
- `esp_ota_img_states_t`：NEW=0、PENDING_VERIFY=1、VALID=2、INVALID=3、ABORTED=4、UNDEFINED=0xFFFFFFFF。
- **`esp_ota_begin` 守卫**：运行分区处于 PENDING_VERIFY 时返回 `ESP_ERR_OTA_ROLLBACK_INVALID_STATE`——未确认的设备无法再次 OTA（含失败重试），除非重启回滚或先 mark valid。TF 卡安装设计必须处理。
- v6.1 `esp_ota_ops.h` 全部所需 API 存在（21 个逐一核实，含 `esp_ota_write_with_offset`、`esp_ota_get_partition_description`、`esp_ota_check_rollback_is_possible`、`esp_ota_get_last_invalid_partition`、`esp_ota_erase_last_boot_app_partition`、`esp_ota_invalidate_inactive_ota_data_slot`）。
- `esp_app_desc_t`：256B，magic 0xABCD5432，位于 **.bin 偏移 0x20**；version[32]/project_name[32]/secure_version/time/date/idf_ver/app_elf_sha256——**manifest 与 BIN 交叉校验的直接依据**；`esp_ota_get_partition_description()` 可读其他槽版本。
- otatool.py（PC 端 USB）：`erase_otadata`、`switch_ota_partition`（写 seq+crc，保留 ota_state）可作救援手段。

### 8.2 现有确认逻辑的缺陷（confirmed，高风险）

`Application::CheckNewVersion`（application.cc:467-554）：最多重试 10 次，延迟 10s 起指数翻倍（最坏累计约 85 分钟，Idle 可提前跳出）；**成功且（无新版本或升级失败）时才调 `MarkCurrentVersionValid`**（ota.cc:260-278，仅 PENDING_VERIFY 时 mark valid，factory 直接 skip）；**10 次全失败 → return，不确认**。

后果：
1. **离线/服务器不可达时，完全健康的固件也保持 PENDING_VERIFY，下次任意重启被回滚**。TF 卡离线安装后同样适用。
2. 确认时机早于唤醒词初始化、早于任何用户交互；**不检查音频/UI/存储/MCP**。
3. `TASK_WDT_PANIC=n` 且 TWDT 只监控 IDLE：主任务 hang 不触发复位 → 不会回滚。
4. `SKIP_VALIDATE_ALWAYS=y`：bootloader 跳过 magic/chip_id/checksum/SHA-256/签名（仍无条件执行段头最小检查与多分区 fallback）；**已 VALID 镜像的 flash 损坏可启动坏镜像 → 砖化**，与 factory 救援目标直接冲突；且该选项 depends on `!SECURE_SIGNED_ON_BOOT`，**与启动时签名校验互斥**。
5. deep-sleep 唤醒走无校验快速路径，Kconfig 明确警告不要与切换 OTA 分区组合——多固件+低功耗场景必须处理（设备是否用 deep sleep = 开放问题）。

### 8.3 回滚改造设计（建议，Phase 6 实施）

- **应用层健康确认，云端不作为必要条件**：本地检查项（显示/触摸初始化、音频 codec 初始化、SD 挂载或明确无卡降级、MCP 工具注册完成、assets 分区有效或内置回退）+ 启动后 N 秒（建议 30-60s）无崩溃 → `esp_ota_mark_app_valid_cancel_rollback()`。云端版本检查保留为独立功能，不再 gating 确认。
- MetaOS UI 增加"确认当前固件/放弃并回滚"手动入口。
- 启动失败计数（NVS）：连续 N 次（建议 3）早期崩溃 → 主动 `esp_ota_mark_app_invalid_rollback_and_reboot()`。
- 评估 `TASK_WDT_PANIC=y`（会把主任务 hang 变成复位→回滚，但影响现有行为，需回归测试）。
- **切换语义**：`esp_ota_set_boot_partition` 对已 VALID 槽也重置 NEW → 每次切换都进入 PENDING_VERIFY 监控 → **主、备固件都必须含确认逻辑**，否则切过去就回不来。factory 不受 OTA 回滚管理（MarkCurrentVersionValid skip factory；bootloader 空白 otadata 回落 factory）。
- `SKIP_VALIDATE_ALWAYS` 改为 `ON_POWER_ON`（上电跳过、其他复位仍校验）或完全关闭；改 defaults:7，需串口重刷 bootloader。启动耗时差异**无实测数据**，不作量化承诺。
- anti-rollback 当前未启用；若未来启用，降级安装受 eFuse secure_version 约束（`ESP_ERR_OTA_SMALL_SEC_VER`），多固件仓库的降级策略需预先考虑。启用不可逆。

### 8.4 签名方案（confirmed 能力 + 建议）

现状：**无任何签名**。Secure Boot/Flash Encryption 未启用；S3 支持 SBv2 **RSA-3072**（3 个 key digest，无 ECC/V1）；`SECURE_SIGNED_APPS_NO_SECURE_BOOT` 未启用。mbedTLS 4.1.0 已启用 ECDSA_C/ECDH_C、SHA 硬件加速；**本 IDF mbedtls 包无 Ed25519**；dependencies.lock 无 libsodium。工程自有代码无密钥（managed_components/espressif__mqtt 示例/测试含厂商测试私钥，与工程无关）。

两条路线：

| | IDF 签名应用（NO_SECURE_BOOT） | 应用层 manifest 签名 |
|---|---|---|
| 算法 | RSA-3072（S3 唯一选项） | ECDSA P-256（PSA API，签名 64B、公钥 64B） |
| 校验点 | `esp_ota_end` 自动（ON_UPDATE 默认 y） | 安装前应用层显式校验 |
| eFuse | 不烧 | 不烧 |
| 信任锚 | **运行中 app 自身签名块的公钥 digest** | 公钥编译内置或 NVS（需设计） |
| 关键约束 | **当前未签名固件无法验证任何签名新固件**（key digest NOT_FOUND）→ 必须先 USB 刷入签名过渡固件 | 无此约束，但需自建公钥分发/轮换机制 |
| 启动时校验 | S3 不可用（ON_BOOT 需 ECDSA V1） | 不涉及 |

**建议**：TF 卡仓库用应用层 ECDSA P-256 签 manifest（含 firmware_sha256、target_chip、target_board、version 等），公钥编译内置；IDF 签名应用作为后续可选加固。**当前不存在可信公钥配置机制，Phase 1 必须先设计密钥生成/保管/内置流程，私钥绝不入固件或仓库。** SHA-256 只证完整性不证身份，二者缺一不可。

其他安全现状（medium）：MCP `self.upgrade_firmware` 接受任意 URL（user_only 仅控制 tools/list 可见性，DoToolCall 无权限检查），http:// 无 TLS；assets 下载单槽覆盖写、16 位累加和校验、`self.assets.set_download_url` 可改写 URL。建议 URL 白名单 + assets 校验加强（A/B 双槽或签名，Phase 4 决策）。

---

## 9. Factory 救援系统设计（建议，Phase 2 实施）

### 9.1 定位与边界

最小化系统：LCD+触摸（**必须初始化 PCA9557**，LCD CS 和功放使能都经它）、SD 扫描、manifest 解析、SHA-256+签名校验、OTA API 安装到 ota_0/ota_1、启动槽选择、状态展示。**不依赖** MiniClaw/Hermes/云端/MCP/音频/ESP-SR。Wi-Fi 可选（建议仅热点配网，去掉 BluFi 省 247KB）。

### 9.2 构建方式（两个选项，Phase 1 决策）

| 选项 | 做法 | 利弊 |
|---|---|---|
| A. 新变体（推荐） | `config.json` builds 增加 `lichuang-dev-rescue`，新 Kconfig 选项（如 `CONFIG_METAOS_RESCUE_MODE`）条件编译排除 MetaOS/BT/摄像头/音频；独立 BOARD_NAME | 复用板级驱动与构建链；符合 AGENTS.md 变体规范；需保证条件编译后仍可独立构建 |
| B. 独立工程 | 新建顶层 rescue 工程目录，共享 boards/common 驱动 | 隔离最彻底；但顶层 CMake 目前单工程、无 EXTRA_COMPONENT_DIRS，基础设施从零搭 |

无论哪种：一次构建一个 `DECLARE_BOARD`；rescue 有自己的板卡身份（OTA 通道独立）；不改现有板卡引脚。

### 9.3 救援入口（confirmed 能力 + 建议）

1. **自动回落**：两个 OTA 槽都无效时 bootloader 自动启动 factory（源码核实）——这是最后防线，无需额外代码。
2. **UI 入口**：MetaOS 设置页"进入救援模式"→ `esp_ota_set_boot_partition(factory)` + 重启（waveshare 板卡有先例）。factory 侧提供"返回主固件"。
3. **启动失败计数**：连续早期崩溃 → 主动切 factory（应用层）。
4. **USB 救援**：otatool `erase_otadata` / `switch_ota_partition`，或 esptool 直刷。
5. **GPIO 长按（BOOTLOADER_FACTORY_RESET）：当前不可行**——bootloader 只能读原生 GPIO；本板 BOOT=GPIO0 是 strapping 脚（复位时拉低进下载模式，与长按复位冲突），音量键=NC，触摸/IO 扩展都在 I2C 上。是否有空闲原生 GPIO 需 Phase 1 核引脚表；默认不采用。注意 FACTORY_RESET 默认擦 nvs（DATA_FACTORY_RESET 默认列表），启用会丢配置，需显式配置。

### 9.4 安装与切换约束

- 安装目标**只允许 ota_0/ota_1**，固件管理器显式禁止 factory 为目标；普通 OTA 的 `get_next_update_partition` 本就不返回 factory。
- 从 factory 运行时 `get_next_update_partition(NULL)` 返回 ota_0；若 ota_0 损坏需应用层判断改写 ota_1（现有 Ota 代码无此逻辑，rescue 需自建选槽：查两槽状态，优先写无效槽）。
- 禁止把 TF 卡 BIN 直接映射执行；禁止写当前运行槽；禁止普通更新流程覆盖 bootloader/分区表/factory。
- 安装走标准 OTA API（`esp_ota_begin/write/end/set_boot_partition`），注意 PENDING_VERIFY 守卫（8.1）；不要为绕过守卫而直写分区（会丢失回滚保护）。
- 写入期间：分块（≤4KB 缓冲）、进度回调、超时、错误码、句柄释放、SD 拔出处理、断电后靠 SHA-256+签名判定而非文件存在。

---

## 10. 潜在风险（核查后）

| # | 风险 | 级别 | 状态 | 证据/说明 |
|---|---|---|---|---|
| 1 | **PNG 解码 abort（现有缺陷）**：1024×1024 PNG → `metaos_image.cc:176` `compressed.resize()` → operator new 失败 → C++ 异常禁用 → abort 重启。日志两次（1657/2388 行），addr2line 全链确认。崩溃时 free_psram≈3.87MB，vector 倍增需旧+新连续块 | high | confirmed | 11-08 提交加的"2MB 空闲检查"**挡不住**此路径（总量够、连续块不够）。raw 缓冲有判空保护，vector 路径没有。修复：限制 IDAT 累积上限/改流式解压/换 heap_caps 分配+判空。发生在 PENDING_VERIFY 期间会触发回滚 |
| 2 | SKIP_VALIDATE_ALWAYS：已 VALID 镜像 flash 损坏可启动坏镜像→砖化；与 factory 回退目标冲突；与启动签名互斥 | high | confirmed | Kconfig 原文警告；esp_image_format.c:334-335。段头最小检查与多分区 fallback 仍在（初审"无任何保护"夸大，已纠正） |
| 3 | 离线不确认→健康固件也被回滚；确认不检查本地子系统 | high | confirmed | application.cc:480-483/526；bootloader_utility.c:393-403 |
| 4 | 候选表原样无 assets → 唤醒词/主题失效 | high（若原样采用） | confirmed | assets.cc:44；srmodels 在 assets。基础文本有内置回退 |
| 5 | 候选表 factory@0x20000 与现网 ota_0 重叠 → OTA 迁移启动错乱 | high（若 OTA 迁移） | confirmed | flasher_args.json；必须串口迁移 |
| 6 | TF 卡无卡检测、mounted_ 状态陈旧、无重挂载 | high（仓库语境） | confirmed | sd_card.cc:43-44/60；拔卡有日志但状态不恢复 |
| 7 | exFAT 不兼容，新 64GB 卡多为 exFAT | high（新卡语境） | confirmed | ffconf.h:294；Kconfig 无选项 |
| 8 | 签名迁移信任锚：未签名现固件无法验签新固件 | high（签名路线） | confirmed | secure_boot_signatures_app.c:156-159；需要 USB 过渡固件 |
| 9 | FS_LOCK=0：已打开文件的并发 unlink/rename 无保护 | medium（潜在） | confirmed | 当前无实际触发路径（DeleteFile/RenameFile 未接入）；固件功能引入后成为真实风险 |
| 10 | max_files=5 共享；notify_http 整首持有 1 槽 | medium | confirmed | 目录操作不占槽（已纠正） |
| 11 | 主任务 SD 阻塞：ListDirectory 无上限+逐文件 stat；TWDT 只监控 IDLE 且不 panic | medium | confirmed | 旧固件 300 项列表触发 task_wdt（日志 755-760） |
| 12 | MCP upgrade_firmware 任意 URL、无签名、http 无 TLS | medium | confirmed | mcp_server.cc:144-164/586-648 |
| 13 | assets 单槽覆盖写、16 位校验和、URL 可被 MCP 改写 | medium | confirmed | assets.cc:495-687；下载中断→分区损坏 |
| 14 | PENDING_VERIFY 守卫阻塞后续 OTA/安装重试 | medium（设计约束） | confirmed | esp_ota_ops.c:178-186 |
| 15 | deep-sleep 无校验快速路径 + OTA 切换组合警告 | medium（若用 deep sleep） | confirmed | Kconfig:336-339；设备是否使用 unknown |
| 16 | NVS 初始化恢复路径有限（仅两种错误自动擦除，其他 abort） | medium（迁移约束） | confirmed | main.cc:17-23；`ESP_ERR_NVS_CORRUPT` 不存在（初审虚构，已纠正） |
| 17 | coredump 死分区 | low | confirmed | MINIMAL_BUILD=ON |
| 18 | GPIO48 双定义（LED/SD CMD） | low（潜在） | confirmed | 本板未实例化 LED |
| 19 | 合并字符串池 238KB 归属失真 | low（分析债务） | confirmed | esp_idf_size 归属 libesp_stdio.a |
| 20 | 16MB 容量未经硬件确认 | unknown | 需 esptool flash_id | bootloader 日志 NONE 隐藏探测 |
| 21 | 原理图 CD/WP、D1-D3 布线 | unknown | oshwhub 客户端渲染未提取 | 官方教程已确认 1 线 |
| 22 | IDF 用 v6.1 标签而非 release 分支（C3 Wi-Fi 问题报告） | unknown（S3 影响） | README_zh.md:121 | 建议 Phase 1 评估切换或压测 |

---

## 11. 具体文件修改清单（计划，Phase 0 未动任何文件）

### Phase 1 分区方案
| 文件 | 动作 | 说明 |
|---|---|---|
| `partitions/v2/16m_metaos.csv`（暂名） | 新建 | 选定方案 A/B/C 之一；不覆盖现有 16m.csv |
| `main/boards/lckfb/szpi-esp32s3/config.json` | 修改 | metaos 变体 sdkconfig_append 增加分区表覆盖（不动 lichuang-dev 原变体） |
| `sdkconfig.defaults` | 修改（经确认） | SKIP_VALIDATE 策略；如启用 coredump/签名/TASK_WDT_PANIC 在此改（生成文件 sdkconfig 不手改） |
| `docs/` 分区映射说明 | 新建 | 偏移/大小/依据/迁移步骤 |

### Phase 2 Factory 救援
| 文件 | 动作 |
|---|---|
| `config.json` | 增加 rescue 变体（唯一 BOARD_NAME） |
| `main/Kconfig.projbuild` | 新增救援模式选项 |
| `main/CMakeLists.txt` | 条件编译排除 MetaOS/BT/摄像头/音频；救援源文件列表 |
| `main/rescue/`（或 components/，Phase 1 定） | 新建：救援应用、仓库扫描、安装器、最小 UI |
| 板卡文档 | 新建 rescue 变体说明（custom-board.md 要求） |

### Phase 3 Firmware Manager
| 文件 | 动作 |
|---|---|
| `main/metaos/metaos_firmware.cc/h`（暂名，遵循现有 MetaOS 模块模式） | 新建：list/validate/install/switch/rollback 状态/进度/错误码；独立安装任务 |
| `scripts/tests/` | 新建主机侧单元测试（manifest 解析、SHA-256、选槽逻辑） |

### Phase 4 TF 仓库
| 文件 | 动作 |
|---|---|
| 仓库扫描/manifest 模块 | 新建（只读扫描、cJSON 解析、所有权规范） |
| `scripts/` 打包签名工具 | 新建：manifest 生成 + ECDSA P-256 离线签名（私钥不入仓库） |
| 公钥内置头 | 新建（密钥流程 Phase 1 设计） |

### Phase 5 UI
| 文件 | 动作 |
|---|---|
| `main/metaos/metaos_panel.cc/h` | Page 枚举扩展 + 4 个新页面（总览/仓库/安装/切换），用现有 Add* 助手；破坏性操作二次确认 |
| `main/metaos/metaos_strings.h` | 新增字符串 |
| 设置页 | 增加"固件管理"与"进入救援模式"入口 |

### Phase 6 回滚与自检
| 文件 | 动作 |
|---|---|
| `main/application.cc` | 健康确认挂点（ActivationTask 后/独立定时器）、启动失败计数 |
| `main/ota.cc` | MarkCurrentVersionValid 解耦云端；选槽增强 |
| `main/main.cc` | 早期失败计数/救援触发（如需） |
| `main/metaos/metaos_image.cc` | **PNG abort 修复**（风险 1，建议提前到 Phase 1 前作为独立 bugfix） |

### Phase 8 文档
`METAOS_FIRMWARE_ARCHITECTURE.md`、`METAOS_PARTITION_LAYOUT.md`、`METAOS_FIRMWARE_MANAGER.md`、`METAOS_SD_FIRMWARE_REPOSITORY.md`、`METAOS_RECOVERY_GUIDE.md`、`METAOS_FIRMWARE_TEST_REPORT.md`（只写真实完成的功能）。

---

## 12. 实施步骤与测试计划

### 阶段门（每阶段完成后停下等确认）

1. **Phase 1**：用户选定分区方案/救援边界/签名路线/skip-validate 策略 → 生成候选 CSV + gen_esp32part 校验 + 映射文档 + 变更计划 → 确认。前置：`esptool flash_id` 确认 16MB（需用户允许接触设备）。
2. **Phase 2**：rescue 变体可独立构建；体积实测对照估算；LCD/触摸/SD/安装/切换最小闭环；不依赖主系统。
3. **Phase 3**：Firmware Manager 主机侧测试先行（`python -m unittest discover -s scripts/tests`），再设备测试。
4. **Phase 4**：只读扫描 → manifest → SHA-256 → 签名 → 分块安装；绝不格式化用户卡。
5. **Phase 5**：4 页 UI，320×240 适配，二次确认。
6. **Phase 6**：健康确认 + 失败回滚 + factory 入口；离线场景必测。
7. **Phase 7**：整机验证（下表）。
8. **Phase 8**：文档交付。

### 整机测试矩阵（Phase 7，对应任务书 20 项）

| # | 用例 | 通过判据 |
|---|---|---|
| 1 | 正常启动主固件 | 健康确认完成，状态 VALID |
| 2/3 | 主↔备切换往返 | 目标固件自行确认；切换后不回滚 |
| 4 | TF 卡安装新固件 | 进度/校验/重启/确认全链 |
| 5 | SHA-256 错误 | 安装前拒绝，错误码+日志 |
| 6 | 签名失败 | 同上 |
| 7 | 不兼容芯片/板型 | manifest+app_desc 双重拒绝（IDF 不查板型，应用层必查） |
| 8 | 超分区大小 | esp_ota_end 拒绝 + 应用层预检 |
| 9 | 无 SD 卡 | 降级启动，救援可用（factory 在内部） |
| 10 | SD 读失败 | 错误码、句柄释放、可重试 |
| 11 | 写入中断电 | **安全方法**：实验台电源加开关，只对目标槽写入阶段断电；绝不破坏运行槽；重启后 SHA-256 判废、可重装；另用 UI 取消+坏 checksum 模拟，不依赖断电 |
| 12 | 新固件启动失败 | 早期计数→回滚/factory |
| 13 | 新固件启动后崩溃 | PENDING_VERIFY→ABORTED→回退（实测） |
| 14 | Factory 救援 | 双槽失效自动回落 + UI 入口 + USB otatool |
| 15 | 连续多次切换 | ≥50 次循环无 otadata 错乱 |
| 16 | NVS 保留 | 迁移前后 WiFi 凭证/设置逐项比对 |
| 17 | 语音回归 | 唤醒/录音/播放/AEC/打断 |
| 18 | 网络回归 | WiFi/WebSocket/MQTT/OTA 检查 |
| 19 | 显示/触摸/音频回归 | 含无显示路径构建 |
| 20 | 长稳 | ≥24h，内存/句柄/SD 状态监控 |

另按 AGENTS.md：核心/协议/音频/CMake 改动跑主机测试 + 代表性芯片构建；协议改动验证 WebSocket 与 MQTT/UDP 两传输；**构建成功不等于硬件验证**，每项注明实测/待测。

---

## 13. 需要用户决策的开放问题（去重后）

1. **分区方案**：A / B / C（推荐 C；B 需 rescue<2MB 承诺）。
2. **nvs 保持 16K**？（推荐保持；候选表 24K 无依据）
3. **rescue 功能边界**：推荐最小集（显示+触摸+SD+安装+切换+状态，Wi-Fi 热点可选，无音频/BT/MetaOS/MCP）。
4. **签名路线**：推荐应用层 ECDSA P-256 签 manifest + 公钥内置；IDF 签名应用（RSA-3072）作后续加固（需 USB 过渡固件）。密钥生成/保管流程 Phase 1 设计。
5. **skip-validate**：推荐改 ON_POWER_ON 或关闭（需串口重刷 bootloader）；启动耗时影响未实测。
6. **健康确认指标与超时**：推荐本地子系统检查 + 30-60s 稳定期，云端不 gating；失败计数阈值 3。
7. **coredump**：现在启用（改 CMake+Kconfig）还是仅预留分区？
8. **迁移方式**：推荐串口局部刷写保留 NVS；已部署设备远程迁移是否必须（两阶段方案高风险）？
9. **deep sleep 是否使用**？影响 skip-validate 与切换组合。
10. **仓库目录**：接受 `/metaos/firmware/...`（大小写不敏感）？现有首次挂载 mkdir 行为是否保留？
11. **rescue 入口**：推荐 UI+自动回落+USB；GPIO 长按需 Phase 1 核空闲引脚（GPIO0 不可用）。
12. **PNG abort 修复**是否作为独立 bugfix 提前执行（推荐是，否则影响回滚测试）？
13. **IDF 标签 vs release/v6.1 分支**：是否切换或压测确认（C3 问题报告，S3 影响未知）？
14. **assets 加固**：A/B 双槽（牺牲 Flash）还是签名/校验加强？（可延后）
15. 允许接触设备执行 `esptool flash_id` 确认 16MB？（Phase 1 前置）

---

## 14. 审计边界声明

- 本审计**未**：修改代码/配置/分区表、编译、烧录、擦除、连接串口、格式化或写入 TF 卡、更新 IDF/依赖、删除文件、改动板卡定义。唯一新增文件是本报告。
- 工作区原有未提交修改（CLAUDE.md）保持原样。
- 设备日志为 2026-11-08 会话的历史记录，本审计期间未产生新设备交互。
- 所有"建议/推荐/估算"均为推断或设计提案，**不是已实现功能**；所有 confirmed 事实附出处可复核。
- 核查员纠正与推翻的初审结论已在正文采用纠正后版本，被推翻项不再出现。

**Phase 0 到此为止。等待用户对第 13 节问题（至少 1-5、8、12、15）确认后，再进入 Phase 1。**
