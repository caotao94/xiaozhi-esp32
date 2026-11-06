# TTS 语音数据设置指南

MetaOS 的提醒播报功能使用 ESP-SR 的离线中文 TTS（"小乐"音色）。

## 准备语音数据

1. **复制语音数据文件到 TF 卡**

   从 ESP-SR 组件目录复制 `xiaole` 语音数据：

   ```bash
   # Windows (在 xiaozhi-esp32 项目根目录下执行)
   copy managed_components\espressif__esp-sr\esp-tts\esp_tts_chinese\esp_tts_voice_data_xiaole.dat [TF卡盘符]:\esp_tts_voice_data_xiaole.dat
   
   # Linux/macOS
   cp managed_components/espressif__esp-sr/esp-tts/esp_tts_chinese/esp_tts_voice_data_xiaole.dat /path/to/sdcard/esp_tts_voice_data_xiaole.dat
   ```

   **文件大小**: 2.9 MB  
   **目标路径**: TF 卡根目录 `/sdcard/esp_tts_voice_data_xiaole.dat`

2. **验证文件**

   启动设备后，在串口日志中查找：

   ```
   I (12345) MetaSpeech: Voice data available: /sdcard/esp_tts_voice_data_xiaole.dat
   ```

   如果看到警告 `Voice data file not found`，说明：
   - 文件路径或文件名不正确
   - TF 卡未正确挂载
   - 文件损坏

## 工作原理

当提醒到时间时：

1. MetaOS 从 TF 卡加载语音数据（2.9MB，加载到 PSRAM）
2. 调用 ESP-SR TTS 合成中文语音（PCM 16kHz 单声道）
3. 使用 Opus 编码器压缩为 24kbps
4. 封装成 Ogg 容器写入 `/sdcard/metaos/speech.ogg`
5. 通过 `Application::StartLocalPlayback()` 播放

整个过程在后台工作线程完成，不阻塞主任务。

## 音质说明

ESP-SR "小乐" 是嵌入式 TTS，音质特点：
- ✅ 清晰度：吐字清楚，提醒内容能听懂
- ⚠️ 自然度：比云端 TTS（如小智服务器）机械
- ✅ 延迟：本地合成，0.5-1 秒内完成短句
- ✅ 离线：不依赖网络

## 故障排查

### 1. 提醒不出声

检查日志：

```
I (xxxxx) MetaSpeech: Synthesizing: 提醒：关火
```

如果没有这条日志，说明提醒功能未触发。

如果有日志但无声音，检查：
- `Voice data file not found` → 语音数据未正确复制
- `Failed to init voice set` → 数据文件损坏，重新复制
- `Failed to create Opus encoder` → 固件编译问题

### 2. 音频文件生成失败

检查 TF 卡空间：

```
I (xxxxx) MetaSpeech: Synthesis complete: 12800 samples, 38400 granules
```

如果看到 `Failed to create output file`，说明：
- TF 卡写保护
- `/sdcard/metaos/` 目录不存在（应该由 MetaOS 自动创建）
- TF 卡已满

### 3. 播放卡顿

正常情况下，短句（<10 字）合成时间 < 1 秒。

如果合成很慢：
- 检查 PSRAM 是否启用（`CONFIG_SPIRAM_USE_MALLOC=y`）
- 检查 TF 卡速度（Class 10 或更快）

## 其他语音数据

ESP-SR 还提供其他音色：

- `esp_tts_voice_data_xiaoxin.dat` (3.7 MB) - "小新" 音色
- `esp_tts_voice_data_xiaoxin_small.dat` (2.8 MB) - "小新"精简版

要更换音色，修改 `main/metaos/metaos_speech.cc`：

```cpp
// 将 xiaole 改为 xiaoxin
#include "esp_tts_voice_xiaoxin.h"
esp_tts_voice_t* voice = esp_tts_voice_set_init(&esp_tts_voice_xiaoxin, voice_data);
```

然后复制对应的 `.dat` 文件到 TF 卡。
