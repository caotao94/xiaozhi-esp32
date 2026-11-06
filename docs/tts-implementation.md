# MetaOS 提醒语音播报实现总结

## 已实现功能

### 1. 完整的离线 TTS 流程

**文件**: `main/metaos/metaos_speech.cc`

- ✅ 从 TF 卡加载 ESP-SR 语音数据（2.9MB，加载到 PSRAM）
- ✅ 使用 ESP-SR TTS 合成中文语音（"小乐"音色）
- ✅ 将 TTS 输出的 PCM 16kHz 单声道音频编码为 Opus 24kbps
- ✅ 封装成 Ogg 容器格式
- ✅ 保存到 `/sdcard/metaos/speech.ogg`
- ✅ 通过 `Application::StartLocalPlayback()` 播放
- ✅ 所有重任务在后台工作线程执行，不阻塞主任务

### 2. 简单 Ogg 封装器

**实现**: `OggWriter` 类（内嵌在 metaos_speech.cc）

- ✅ 写入 OpusHead 和 OpusTags 头部
- ✅ 写入 Opus 数据包到 Ogg 页
- ✅ 正确计算 CRC32 校验和
- ✅ 设置 BOS/EOS 标志

### 3. 图片查看器和姿态手势

之前 Agent 实现的功能（已修复）：

- ✅ JPEG/PNG/GIF/BMP 图片查看
- ✅ QMI8658 IMU 驱动
- ✅ 摇一摇切歌、翻转停止

### 4. 文档和工具

- ✅ `docs/tts-setup.md` - 用户设置指南
- ✅ `scripts/prepare_tts_data.py` - 语音数据复制脚本

## 技术细节

### TTS 参数

```cpp
ESP-SR TTS:
  - 采样率: 16kHz
  - 位深: 16-bit
  - 声道: 单声道
  - 速度: 0 (最慢，最清晰)
  - 音色: xiaole (小乐)

Opus 编码:
  - 采样率: 16kHz
  - 比特率: 24kbps
  - 帧长: 20ms
  - 应用模式: VOIP
  - 复杂度: 0 (最快)
```

### 内存使用

- 语音数据: 2.9MB (PSRAM)
- TTS 状态: ~100KB (内部分配)
- Opus 编码器: ~20KB
- 输出缓冲: ~1KB/帧
- 总计: ~3MB PSRAM + ~50KB 内存

### 文件大小估算

提醒文字 "关火" (2个字):
- TTS 输出: ~0.5秒 PCM = 16KB
- Opus 压缩: ~1.5KB
- Ogg 容器: ~2KB

提醒文字 "下午三点开会，地点在会议室" (13个字):
- TTS 输出: ~3秒 PCM = 96KB
- Opus 压缩: ~9KB
- Ogg 容器: ~10KB

## 工作流程

1. **提醒到时间** → `MetaOs::CheckReminders()` 检测到
2. **调用 TTS** → `SpeechSynthesizer::SynthesizeAndPlay(messages)`
3. **后台合成** → 工作线程执行:
   - 加载语音数据
   - 创建 TTS 和 Opus 编码器
   - 流式合成并编码
   - 写入 Ogg 文件
4. **回到主任务** → `Application::Schedule()` 调用 `StartLocalPlayback()`
5. **播放** → 音频服务解码 Ogg/Opus 并播放

## 测试步骤

### 前置条件

1. 烧录固件到设备
2. 复制语音数据到 TF 卡根目录:
   ```bash
   python scripts/prepare_tts_data.py D:/
   ```

### 功能测试

1. **验证语音数据加载**
   - 启动设备
   - 查看串口日志: `Voice data available: /sdcard/esp_tts_voice_data_xiaole.dat`

2. **创建测试提醒**
   - 设置一个 1 分钟后的提醒: "测试"
   - 等待提醒到时间

3. **验证播报**
   - 应该听到: "提醒：测试"
   - 检查日志:
     ```
     I MetaSpeech: Synthesizing: 提醒：测试
     I MetaSpeech: Synthesis complete: XXXX samples, YYYY granules
     ```

4. **验证文件生成**
   - 检查 TF 卡: `/sdcard/metaos/speech.ogg`
   - 可以复制到电脑用播放器验证

### 异常测试

1. **无语音数据文件**
   - 删除 TF 卡上的 .dat 文件
   - 提醒到时间应该只有弹窗，无声音
   - 日志: `Voice data file not found`

2. **TF 卡空间不足**
   - 填满 TF 卡
   - 日志: `Failed to create output file`

3. **网络断开**
   - 断开 WiFi
   - 提醒仍应正常播报（离线功能）

## 已知限制

1. **首次合成延迟**: 加载 2.9MB 语音数据需要 ~200ms
2. **不支持超长文本**: 限制 200 字节（约 60 个汉字）
3. **音质**: 机械感比云端 TTS 强，但清晰度足够
4. **内存要求**: 需要 PSRAM，否则无法加载语音数据

## 优化方向（未实现）

1. **预加载语音数据**: 启动时加载并缓存，避免每次提醒时重新加载
2. **语音缓存**: 常见提醒文字可以预合成并缓存
3. **可变速度**: 支持用户选择语速（0-5）
4. **其他音色**: 支持切换到 xiaoxin
5. **SSML 支持**: 支持停顿、重音等标记

## 文件清单

```
main/metaos/metaos_speech.cc          # TTS 实现
main/metaos/metaos_speech.h           # TTS 接口
docs/tts-setup.md                     # 用户指南
scripts/prepare_tts_data.py           # 数据准备脚本
managed_components/.../xiaole.dat     # 语音数据（2.9MB）
```

## 相关组件

- **ESP-SR**: espressif__esp-sr (managed component)
- **Opus 编码器**: espressif__esp_audio_codec
- **Ogg 解码器**: main/audio/demuxer/ogg_demuxer.cc (已有)
- **音频服务**: main/audio/audio_service.cc
