# MetaOS 多固件改造 Phase 1 变更计划

- 日期：2026-11-12
- 依据：METAOS_FIRMWARE_MIGRATION_AUDIT.md（Phase 0，已确认）
- 状态：**计划待确认。除已批准的 PNG 独立 bugfix 和新建候选分区表文件外，未改任何配置/代码。未烧录。**

## 0. 已确认决策（用户 2026-11-12）

| 项 | 决策 |
|---|---|
| 分区方案 | **C**：factory/ota_0/ota_1 各 4MB + assets 3840K + coredump 128K |
| 救援固件边界 | 最小集：屏幕+触摸+SD+安装/切换+状态；Wi-Fi 热点可选；无音频/蓝牙/MetaOS/MCP/云依赖 |
| 签名 | 应用层 ECDSA P-256 签 manifest，公钥编译内置；IDF 签名应用后续可选 |
| skip-validate | `SKIP_VALIDATE_ALWAYS` → `SKIP_VALIDATE_ON_POWER_ON` |
| 迁移 | 串口局部刷写，保留 NVS |
| PNG 崩溃 | 作为独立 bugfix 提前修（已批准） |
| 深度睡眠 | **不使用**（用户确认）→ deep-sleep 无校验路径顾虑消除 |
| 设备数量 | **仅 1 台**（用户确认）→ 无需远程/两阶段迁移方案 |
| 设备访问 | 已授权只读 `flash_id`（已执行）；烧录仍需逐次确认 |

## 1. 前置检查结果

### 1.1 Flash 硬件确认（已执行，只读）

`esptool --chip esp32s3 -p COM7 flash-id`：
- **Detected flash size: 16MB** ✓（审计 unknown #20 关闭）
- quad（4 数据线），3.3V，Manufacturer 0x5E / Device 0x4018
- 与 sdkconfig `ESPTOOLPY_FLASHSIZE=16MB`、QIO 80MHz 一致

### 1.2 候选分区表校验（已执行）

新文件：`partitions/v2/16m_metaos.csv`（**未激活**，现有 16m.csv 未动）。

- `gen_esp32part.py --flash-size 16MB` → exit 0 ✓
- `--secure v2` → exit 0 ✓
- bin→CSV 往返一致 ✓
- 余量：app 槽 4,194,304B − 固件 3,312,448B = **881,856B（21.0%）**；assets 3,932,160B − 下载后 2,266,376B = **1,665,784B（×1.73）**；总占用恰为 0x1000000，无尾部空闲

### 1.3 救援入口 GPIO 核对（已执行，只读）

本板 GPIO 占用（config.h + lichuang_dev_board.cc）：
- 0=BOOT（strapping，复位拉低进下载模式，**不能**做长按救援）；1/2=I2C（与摄像头共享）；3-9、15-18=摄像头；12-14=I2S；21=SD D0；38=I2S MCLK；39-41=LCD SPI；42=背光；45=I2S DOUT；46=摄像头 HREF；47/48=SD CLK/CMD
- LCD reset=NC，触摸 INT/RST=NC，音量键=NC
- S3 模组固有占用：26-32=SPI Flash，33-37=Octal PSRAM（不可用）；19/20=USB；43/44=UART0 控制台
- **空闲原生 GPIO：仅 10、11**（代码无引用；是否引出到排针 unknown，原理图托管 oshwhub 未能提取）

结论：**不启用 BOOTLOADER_FACTORY_RESET**（无合适按键；GPIO0 冲突）。救援入口采用：
1. 双 OTA 槽均无效时 bootloader 自动回落 factory（IDF 内置，源码已核实）
2. MetaOS 设置页"进入救援模式"→ `esp_ota_set_boot_partition(factory)` + 重启（waveshare 板卡有先例）
3. 应用层启动失败计数 → 主动切 factory
4. USB：otatool `erase_otadata` / `switch_ota_partition`、esptool 直刷
5. （可选，不在默认方案）若用户愿在 GPIO10/11 飞线按键且确认引出，可后续启用 FACTORY_RESET；注意其默认擦 nvs，需显式配置 DATA_FACTORY_RESET

## 2. 分区映射（方案 C）

| 分区 | 类型 | 偏移 | 大小 | 与现网对比 | 依据 |
|---|---|---|---|---|---|
| nvs | data/nvs | 0x9000 | 16K | **不变** | 数据零迁移；无扩容依据 |
| otadata | data/ota | 0xd000 | 8K | **不变** | 迁移只需写初始值 |
| phy_init | data/phy | 0xf000 | 4K | **不变** | 未使用，保留兼容 |
| factory | app/factory | 0x20000 | 4M | 新增（占原 ota_0 位置） | 救援固件估算 1.1-1.3MB，余量充足 |
| ota_0 | app/ota_0 | 0x420000 | 4M | 偏移后移 | 主固件，21% 余量 |
| ota_1 | app/ota_1 | 0x820000 | 4M | 偏移后移 | 备用固件 |
| assets | data/spiffs | 0xc20000 | 3840K | 缩小（原 8M@0x800000） | 下载后 2.27MB，×1.73 余量 |
| coredump | data/coredump | 0xfe0000 | 128K | 新增 | 预留；启用需 CMake 纳入 espcoredump + Kconfig（MINIMAL_BUILD=ON 当前不构建该组件） |

关键约束（审计已核实）：
- factory@0x20000 与现网 ota_0 重叠 → **现网设备不能 OTA 迁移**，必须串口刷写（本机仅 1 台，可行）
- `SPI_FLASH_DANGEROUS_WRITE_ABORTS=y`：应用层写分区表/bootloader 区会 abort，迁移只能串口
- assets 缩小后仍大于下载后尺寸；`Assets::Download` 有 content_length>分区大小的拒绝检查，未来更大的包会被拒绝（可接受，报错而非损坏）

## 3. Bootloader / 回滚配置变更

| 配置 | 现值 | 改为 | 位置 | 影响 |
|---|---|---|---|---|
| BOOTLOADER_SKIP_VALIDATE_ALWAYS | y | **n** | sdkconfig.defaults:7 | 恢复镜像校验；坏镜像可被 bootloader 拒绝并回落 |
| BOOTLOADER_SKIP_VALIDATE_ON_POWER_ON | y（被 ALWAYS select） | **y（显式）** | sdkconfig.defaults | 上电跳过校验保启动速度，崩溃/看门狗复位仍校验 |
| BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP | y（被 ALWAYS select） | 跟随 Kconfig（设备不用深睡，无影响） | — | 实现时核实 select 关系 |
| BOOTLOADER_APP_ROLLBACK_ENABLE | y | **保持 y** | sdkconfig.defaults:8 | 回滚机制基础 |
| BOOTLOADER_APP_ANTI_ROLLBACK | n | 保持 n | — | 多固件仓库需降级安装；启用会限制降级且不可逆 |
| BOOTLOADER_FACTORY_RESET | n | 保持 n | — | 无合适 GPIO（1.3） |
| BOOTLOADER_LOG_LEVEL | NONE | 迁移调试期临时 INFO，验证后改回 | sdkconfig.defaults:6 | 观察分区选择日志 |
| ESP_TASK_WDT_PANIC | n | **Phase 6 评估**（倾向 y） | — | 主任务 hang→复位→回滚；改变现有行为，需回归 |

- bootloader 配置变更 → **必须串口重刷 bootloader.bin**（OTA 不更新 bootloader，IDF 设计）
- 体积：当前 16,736B / 上限 0x8000，余量 16,032B；ON_POWER_ON 不新增校验代码（同一套逻辑的子集），无体积风险
- 签名互斥说明：SKIP_VALIDATE 选项 depends on !SECURE_SIGNED_ON_BOOT；本方案签名在应用层（esp_ota_end 之后、安装前），不启用 ON_BOOT 签名，无冲突

## 4. 迁移步骤（单台，串口，保留 NVS）

**执行时机：Phase 2 救援固件构建完成后。烧录前再次向用户确认。**

1. 备份（只读）：
   ```
   esptool --chip esp32s3 -p COM7 read_flash 0x9000 0x4000 nvs_backup.bin
   esptool --chip esp32s3 -p COM7 read_flash 0xd000 0x2000 otadata_backup.bin
   ```
2. 构建：rescue 变体（新表）+ metaos 主变体（新表，含 PNG 修复）+ bootloader（ON_POWER_ON）
3. 局部刷写（**不擦 0x9000-0xCFFF**）：
   ```
   esptool --chip esp32s3 -p COM7 -b 460800 write_flash \
     --flash_mode dio --flash_freq 80m --flash_size 16MB \
     0x0      build/bootloader/bootloader.bin \
     0x8000   build/partition_table/partition-table.bin \
     0xd000   build/ota_data_initial.bin \
     0x20000  build/rescue.bin        (factory) \
     0x42000  build/xiaozhi.bin       (ota_0) \
     0xc20000 build/generated_assets.bin  (assets)
   ```
   （具体文件名以 Phase 2 构建产物为准；assets 用默认包或用户已验证的官方包，二者内容等价）
4. 首次启动预期：otadata 全 FF + 有 factory → **bootloader 启动 rescue**（正好验证救援路径）
5. 在 rescue UI 选择"启动 ota_0"→ set_boot_partition → NEW → PENDING_VERIFY → 主固件健康确认（Phase 6 实现前，主固件沿用现有云端确认逻辑；离线时注意会回滚——迁移验证阶段保持联网）
6. 验证：WiFi 凭证仍在（NVS 保留）、唤醒词可用（assets）、显示/触摸/音频正常、rescue↔main 往返切换
7. 回退预案：若新表启动异常，串口刷回旧表+旧固件（备份的 nvs 不受影响）；旧表产物在 git 历史与当前 build/ 中可恢复

**不做**：全片擦除（会丢 NVS）；OTA 迁移（重叠分区，必砖）；自动格式化 TF 卡。

## 5. 签名密钥流程设计（Phase 4 实现，此处定方案）

现状：工程无任何签名/公钥机制（审计 confirmed）。mbedTLS 4.1.0，ECDSA_C 已启用，SHA 硬件加速；**无 Ed25519**；legacy ecdsa.h 已私有化 → 用 **PSA API**（psa_verify_hash / psa_import_key）。

- **算法**：ECDSA P-256 + SHA-256。签名 64B（r||s），裸公钥 64B（x||y）
- **密钥生成**：PC 离线，`openssl ecparam -name prime256v1 -genkey -noout -out metaos_fw_signing.key`（或 python cryptography）。**私钥不入仓库、不入固件、不入 TF 卡**；由用户离线保管（建议密码管理器/加密存储）
- **公钥内置**：脚本从私钥导出裸公钥 → 生成 `main/metaos/metaos_firmware_pubkey.h`（64 字节常量数组 + key_id）。该头文件入仓库（公钥可公开）；生成脚本入 scripts/
- **签名对象**：manifest.json 的**精确文件字节**，签名放独立文件 `manifest.json.sig`（64B 裸签名）。避免 JSON 规范化陷阱；manifest 内含 firmware_sha256，信任链 = 公钥验 manifest 签名 → manifest 的 sha256 验 BIN
- **manifest 必备字段**（对齐任务书）：firmware_id、firmware_name、version、build_number、target_chip、target_board、min_bootloader_version、firmware_size、firmware_sha256、signature（或 detached .sig）、release_channel、release_time、description、compatibility、rollback_supported；另加 key_id（轮换预留）、manifest_version
- **设备校验顺序**：解析 manifest → PSA 验签（.sig 对 manifest 字节）→ 读 BIN 算 SHA-256 比对 → 校验 target_chip/board（app_desc @ bin+0x20 交叉核对 project_name/version）→ 尺寸≤目标槽 → 才允许安装
- **轮换**：v1 单公钥；key_id 字段预留多公钥数组扩展
- **诚实声明**：此机制建立之前，TF 仓库安装只具备 SHA-256 完整性校验，**不具备发布者认证**；UI 必须如实显示"未签名/签名无效"状态，不得假装已认证

## 6. 救援变体与模块放置（Phase 2/3 实现，此处定方案）

- **rescue 构建**：`main/boards/lckfb/szpi-esp32s3/config.json` builds 增加 `lichuang-dev-rescue`；新 Kconfig 选项（如 `CONFIG_METAOS_RESCUE_MODE`）；main/CMakeLists.txt 条件编译排除 MetaOS 面板/音频/ESP-SR/蓝牙/摄像头/MQTT/ML307；独立 BOARD_NAME（OTA 身份独立，AGENTS.md 要求）；一次构建一个 DECLARE_BOARD；不改现有板卡引脚
- **rescue 功能**：PCA9557+LCD+触摸初始化（LCD CS 经 PCA9557，必须带）、SD 挂载（复用 boards/common/sd_card）、仓库只读扫描、manifest+SHA-256+签名校验、OTA API 安装到 ota_0/ota_1（禁止 factory 为目标）、启动槽选择、状态页；Wi-Fi 热点配网可选（去 BluFi 省 247KB）；内置 basic 字体回退，不依赖 assets
- **Firmware Manager（主固件侧）**：`main/metaos/metaos_firmware.cc/h`，遵循现有 MetaOS 模块模式（main/metaos 是 owning layer；不放 components/——AGENTS.md 将其列为 vendor/生成目录）。独立安装任务（**不复用** metaos_work 深度 4 队满丢弃的队列），分块≤4KB，进度/错误码/超时/句柄释放
- **UI**：metaos_panel Page 枚举扩展 4 页（总览/仓库/安装/切换），用现有 Add* 助手，320×240，破坏性操作二次确认；设置页加"固件管理"+"进入救援模式"入口

## 7. 健康确认默认参数（Phase 6 实现）

- 本地检查项：显示/触摸初始化、音频 codec 初始化、SD 挂载或明确无卡降级、MCP 工具注册完成、assets 有效或内置回退
- 稳定期：启动后 **45s**（30-60s 区间取中）无崩溃 → mark valid；**云端检查不作为必要条件**（解耦现有 CheckNewVersion gating）
- 启动失败计数（NVS）：连续 **3** 次早期崩溃 → mark_invalid_rollback_and_reboot
- UI 手动"确认/回滚"入口
- 主、备固件都含确认逻辑（set_boot_partition 对已 VALID 槽也重置 NEW，审计已核实）

## 8. 文件变更清单（按阶段，均未执行）

| 阶段 | 文件 | 动作 |
|---|---|---|
| 1（本阶段产物） | partitions/v2/16m_metaos.csv | **已新建**（候选，未激活） |
| 1 | METAOS_PHASE1_CHANGE_PLAN.md | **已新建**（本文档） |
| 独立 bugfix | main/metaos/metaos_image.cc | **已修改**（PNG 解码修复，已批准；构建通过 2026-11-12，3,313,088B；独立审查完成，见 §11） |
| 2 | config.json / main/Kconfig.projbuild / main/CMakeLists.txt | rescue 变体 + 条件编译 |
| 2 | main/rescue/（新目录） | 救援应用 |
| 2 | sdkconfig.defaults | skip-validate 改 ON_POWER_ON（**需确认**） |
| 2 | metaos 变体 config.json | sdkconfig_append 增加 PARTITION_TABLE_CUSTOM_FILENAME="partitions/v2/16m_metaos.csv"（**需确认**；不动 lichuang-dev 原变体） |
| 3 | main/metaos/metaos_firmware.cc/h + scripts/tests/ | 管理模块 + 主机测试 |
| 4 | 仓库扫描/manifest 模块、scripts/ 打包签名工具、公钥头 | 新建 |
| 5 | metaos_panel.cc/h、metaos_strings.h | 4 页 UI + 入口 |
| 6 | application.cc、ota.cc、main.cc | 健康确认 + 失败计数 |
| 6 | sdkconfig.defaults | TASK_WDT_PANIC 评估（需确认） |
| 8 | 6 份交付文档 | 只写真实完成功能 |

## 9. Phase 1 验证计划

- [x] flash_id 确认 16MB
- [x] 候选表 gen_esp32part 校验（plain + secure v2 + 往返）
- [x] 固件/assets 余量核算
- [x] GPIO 核对
- [x] PNG 修复构建通过（2026-11-12，xiaozhi.bin 3,313,088B，IDF_EXIT=0，clang-format 干净）
- [x] 独立对抗式代码审查完成（见 §11），发现的问题已全部修复并重新构建
- [ ] PNG 修复硬件验证（烧录需确认）：① 此前崩溃的 1024×1024 PNG（/sdcard/pic/00012-*.png 等）应正常解码或报"内存不足"，不再 abort；② 普通小 PNG 应正常显示（验证 zlib 标志修复）；③ 灰度+alpha PNG 应正常显示；④ 截断 PNG 应报"文件损坏"且工作线程不死循环（TTS/提醒仍可用）
- [ ] 用户确认本计划 → 进入 Phase 2

## 11. PNG 解码修复与独立审查记录

修复范围（均在 DecodePng 内，已批准的独立 bugfix）：

| # | 问题 | 来源 | 修复 |
|---|---|---|---|
| 1 | std::vector 扩容→operator new 失败→abort（设备日志两次崩溃的根因） | 用户报告+addr2line | 两遍扫描+单次 heap_caps_malloc+判空 |
| 2 | 旧 resize 对恶意 chunk.length 的 32 位回绕堆溢出（潜在） | 审查发现 | 每 chunk 8MB 上限 |
| 3 | raw 分配失败泄漏 compressed（最多 8MB/次） | **第一版修复引入的回归**，审查抓出 | 补 heap_caps_free |
| 4 | tinfl 缺 TINFL_FLAG_PARSE_ZLIB_HEADER：IDAT 是 zlib 流，按 ROM 契约（esp_rom/include/miniz.h:540，IDF ROM 测试同用法）缺标志当 raw deflate 解析→几乎全部 PNG 失败 | 原有，审查发现，主会话独立核对契约 | 加标志 |
| 5 | inflate 循环 HAS_MORE_INPUT 永不清除+无零进展守卫→截断流/输出缓冲不足时死循环，共享工作线程挂起→TTS/提醒停摆 | 原有，审查发现 | 输入耗尽清标志+零进展即 kCorrupt |
| 6 | color_type 4（灰度+alpha）bpp 算成 1（应 2）→raw_size 减半→触发 #5；且无转换分支 | 原有 | bpp=2+转换分支（忽略 alpha） |
| 7 | 调色板(3)/亚字节深度未拒绝→黑图或读未初始化行；零尺寸未拒绝 | 原有 | 明确 kUnsupported/kCorrupt |
| 8 | 解压不完整仅告警→用未初始化 PSRAM 渲染 | 原有 | kCorrupt |
| 9 | 第二遍扫描无上限（TOCTOU）；IHDR 长度/seek 未校验；filter>4 静默当 None；make_unique 抛异常路径 | 原有/加固 | 全部补齐 |

审查同时确认：两遍偏移算术正确、无双重释放/ use-after-free、compressed 释放后无引用、整数无溢出（4096 上限内）、tinfl 非回绕缓冲调用正确、32KB 工作栈容纳 ~11KB tinfl 状态。8MB IDAT 上限为合理取舍（更大的图解压后也超 8MB PSRAM，本就不可解），已注释说明。

**残留已知项**：JPEG/GIF/BMP 解码器及本函数外的 make_unique 仍有极端内部 RAM 耗尽下的 abort 路径（24B 级分配，风险可忽略，与本次多 MB 崩溃不同性质）；adler32 校验依赖 ROM 行为，未单独验证。

**构建不能证明以上修复正确——硬件验证清单见 §9，烧录需用户确认。**

## 10. Phase 1 当时的边界（已被 Phase 2 推进）

Phase 1 仅产出候选分区表与计划，未激活配置、未改 bootloader、未烧录、未碰 TF 卡。
这些限制在用户确认计划后由 Phase 2 解除（见 §12）。flash_id 为已授权只读操作。

## 12. Phase 2 完成记录（Factory 救援固件，2026-11-12）

### 交付物（均编译通过，未硬件验证）

| 变体 | 体积 | 槽余量 | 配置确认 |
|---|---|---|---|
| lichuang-dev-rescue | 746 KB (0xba9b0) | 4MB 槽 82% | RESCUE_MODE=y，英文 UI，无音频/蓝牙/云 |
| lichuang-dev-metaos | 3.13 MB (0x3220a0) | 4MB 槽 22% | ZH_CN=y、BT/BluFi=y、METAOS=y、新分区表、含 PNG 修复 |

新增文件：`main/rescue/{rescue_main,rescue_ui}.{h,cc}`、`main/firmware/{firmware_repo,firmware_install}.{h,cc}`、
`main/boards/lckfb/szpi-esp32s3/lichuang_rescue_board.{h,cc}`、`partitions/v2/16m_metaos.csv`。
修改：CMakeLists.txt（rescue 源分流）、Kconfig.projbuild（RESCUE_MODE + 互斥）、config.json（两个变体）、
main.cc（入口分流）、lichuang_dev_board.cc（#if 守卫）、sdkconfig.defaults（skip-validate→ON_POWER_ON）、
metaos_image.cc（PNG 修复）、README_metaos.md（救援文档）。

### 构建要点（踩坑记录）

- ESP-IDF 不强制 include sdkconfig.h：文件级 `#if CONFIG_` 前必须先 `#include "sdkconfig.h"`，否则宏未定义整文件被跳过（曾导致救援对象为空）。
- 语言由 build.py `--language` 控制（默认 en-US），会覆盖 config.json 的语言项；主变体须 `--language zh-CN`。
- mbedTLS 4.1.0 旧头文件私有化，SHA-256 用 PSA API（psa_hash_*）。
- esp_lvgl_port 2.9.0 显示接口为 `lvgl_port_add_disp`（非 add_display），头文件 esp_lvgl_port_disp.h。
- 从 Git Bash 调用构建须先 `Remove-Item Env:MSYSTEM`，否则 idf.py 走 MSYS 分支不执行。

### 独立审查（3 维 + 对抗核查，6 agent）与修复

审查确认 4 critical / 5 major，全部修复：
1. 板卡兼容检查误用救援自身 BOARD_NAME → 改 BOARD_TYPE（主固件可安装）。
2. UI 在主线任务无锁构建 → 全部 LVGL 操作持锁/入 LVGL 任务。
3. 安装任务 8KB 栈溢出 → 16KB + IO 缓冲堆分配。
4. 中文缺字 → 救援 UI 改英文（basic 字体覆盖 ASCII）。
5. 扫描阻塞 LVGL 任务 + Mount 跨任务 → 单一常驻工作任务统一挂载/扫描/安装。
6. 非模态对话框致"确认A装B"/安装中误切换 → 全模态 + 安装期间阻断输入 + 条目/目标快照。
7. 校验阶段不可取消 + 取消竞态 → 两阶段进度（verify/write）+ 启动前复位取消标志。
另修：manifest 尺寸交叉校验、bin 路径防穿越、JSON 数值 UB、嵌套深度防护、SHA 摘要长度断言、
app_desc 版本核对、救援自确认防回滚、xTaskCreate 返回值检查、Switch 失败弹框、触摸句柄泄漏、
PCA9557 关 PA/摄像头、去 board.h 依赖、Kconfig 互斥、cJSON 显式依赖、头文件 sdkconfig.h。

### 已知限制（如实记录）

- 救援 UI 英文；中文子集字体留待后续独立任务。
- 签名验证为 Phase 4，当前仅 SHA-256，UI 标注"未验证"。
- 卡拔出后 mounted_ 不清零（SdmmcCard 限制），救援换卡需重启。
- 主固件离线不确认→回滚的缺陷属 Phase 6（审计 §8.2 已记录）。
- 组件依赖图未完全裁剪（PRIV_REQUIRES 仍含 bt 等，靠变体 BT_ENABLED=n 关闭）——精简优化项，非正确性问题。
- **全部为构建级验证；救援启动、安装、切换、PNG 修复均需硬件实测（随迁移烧录进行，需用户确认）。**

### 下一步

等待用户确认 Phase 2 → 进入 Phase 3（主固件侧 Firmware Manager，复用 main/firmware 核心）。
迁移烧录（新分区表 + rescue + 主固件 + PNG 验证）需用户单独确认。

## 13. Phase 3-6 完成记录（2026-11-12）

### 交付物（均编译通过 + 主机测试通过，未硬件验证）

| 变体 | 体积 | 4MB 槽余量 |
|---|---|---|
| lichuang-dev-metaos | 3.15 MB (0x327c40) | 21% |
| lichuang-dev-rescue | 790 KB (0xc5770) | 81% |

主机测试：scripts/tests/test_firmware_package.py 6/6 通过（密钥生成、manifest 生成、签名/验签往返、防篡改 manifest/bin、非镜像拒绝）。

### 各阶段实现

- **Phase 3 固件管理模块**：main/metaos/metaos_firmware.{h,cc}（FirmwareManager，单一工作线程做 SD 扫描+OTA 安装，主任务 RequestScan/RequestInstall/Cancel/Poll，原子状态+互斥暂存）。复用 main/firmware 核心。MetaOs 持有并在 Initialize/OnTick 集成。
- **Phase 4 TF 仓库+签名**：firmware_repo VerifySignature（PSA ECDSA P-256/SHA-256，内嵌公钥 firmware_pubkey.h，detached manifest.json.sig 64B r||s）；Install 签名门（无效签名始终拒绝，CONFIG_METAOS_FW_REQUIRE_SIGNATURE 控制是否强制）；scripts/firmware_package.py（genkey/package/verify，私钥不入仓库）。开发密钥已生成（私钥在仓库外临时目录，生产需用户自生成）。
- **Phase 5 MetaOS UI**：metaos_panel.cc BuildFirmware/RefreshFirmware（Page::kFirmware，概览/仓库/安装/切换/进度，设置页入口），模态确认、目标槽禁用运行槽、安装进度+取消。
- **Phase 6 启动健康+回滚**：application.cc ConfirmFirmwareHealth（45s 定时器，本地检查独立于云端，成功 MarkCurrentVersionValid+清零 boot_fail）；ota.cc MarkCurrentVersionInvalid；main.cc 启动失败计数（仅 PENDING_VERIFY+异常复位计数，>=3 回滚，先清零防乒乓，救援排除）。

### 独立审查（3 维+对抗核查）与修复

审查发现 1 critical + 5 major + 多 minor，全部修复：
- **critical**：Install 读 288B 镜像头后未回绕文件指针 → 写循环从偏移 288 开始 → 每次安装必失败且目标槽被擦除。已加 fseek(0)。
- **major**：busy() 含终态防结果丢失；boot_fail 仅 PENDING_VERIFY+异常复位（健康固件不误回滚）；签名展示用 has_signature；Poll install/scan 错误分离。
- **minor**：读满 288B 头、manifest 重解析交叉校验、JsonU64 严格 <2^64、签名路径守卫+严格 64B+key_id 校验、PSA init 前置、取消正确报 cancelled、升级延迟上限、MarkValid 返回 bool 成功才清零、Start 返回值检查、指针空初始化、UI 切换前重验 busy/面板关闭丢弃回调/安装中跳过概览刷新/取消按钮成员定位/扫描期隐藏取消。

### 已知限制（如实记录，待硬件验证或后续）

- 全部构建级验证；扫描/安装/切换/取消/回滚/健康确认时机均需随迁移烧录实测。
- 签名用开发密钥；生产部署前用户须用 firmware_package.py genkey 生成自己的密钥并替换公钥头。
- 对话框在较大主题字体下可能溢出（FW-UI-07），需上机复核布局。
- 半写入槽位的 valid_image 仅查 app_desc magic，可能误显"有效"（bootloader 校验兜底）。
- 安装失败告警与提醒告警单槽互斥，可能互相覆盖（既有 alert 机制限制）。
- 组件依赖图未完全裁剪（精简优化项，非正确性）。

### 下一步（Phase 7，需用户确认）

硬件验证 + 分区迁移烧录（新分区表 + rescue + 主固件，串口局部刷写保留 NVS）。
验证清单见 §9 + 任务书 Phase 7 的 20 项。烧录是写设备操作，需用户单独授权。
