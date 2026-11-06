# XiaoZhi ESP32 开发环境配置

## ESP-IDF 环境激活

在 PowerShell 中运行以下命令激活开发环境：

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
```

## 环境路径

- **ESP-IDF**: `E:\esp\v6.1\esp-idf`
- **工具路径**: `C:\Espressif\tools`
- **Python**: `C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe`
- **串口**: `COM7`
- **目标芯片**: ESP32-S3

## 常用命令

### 编译
```powershell
idf.py build
```

### 烧录
```powershell
idf.py -p COM7 flash
```

### 监视串口
```powershell
idf.py -p COM7 monitor
```

### 烧录并监视
```powershell
idf.py -p COM7 flash monitor
```

### 清理构建
```powershell
idf.py fullclean
```

## 项目配置

当前分支: `metaos-v2-fixes`

主要功能:
- MetaOS V2 面板系统
- 提醒管理和离线 TTS 播报
- 图片查看器 (JPEG/PNG/GIF/BMP)
- IMU 姿态手势 (摇一摇/翻转)
- 音乐播放器
- 文件浏览器

## TTS 语音数据

提醒播报需要语音数据文件，复制到 TF 卡：

```bash
python scripts/prepare_tts_data.py D:/
```

或手动复制：
```
源文件: managed_components\espressif__esp-sr\esp-tts\esp_tts_chinese\esp_tts_voice_data_xiaole.dat
目标: TF卡根目录\esp_tts_voice_data_xiaole.dat
```

详见: `docs/tts-setup.md`

## VSCode 配置

`.vscode/settings.json` 中的配置：

```json
{
    "idf.espIdfPath": "E:\\esp\\v6.1\\esp-idf",
    "idf.toolsPath": "C:\\Espressif\\tools",
    "idf.pythonBinPath": "C:\\Espressif\\tools\\python\\v6.1\\venv\\Scripts\\python.exe",
    "idf.port": "COM7"
}
```

**注意**: 如果 VSCode 配置中 `pythonBinPath` 指向 `E:\Espressif`，需要改为 `C:\Espressif`。

## 故障排查

### idf.py 命令找不到

确保已激活环境：
```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
```

### Python 虚拟环境路径错误

手动设置环境变量：
```powershell
$env:IDF_PATH = "E:\esp\v6.1\esp-idf"
$env:IDF_TOOLS_PATH = "C:\Espressif\tools"
```

### 直接调用（无需激活环境）

```powershell
C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe E:\esp\v6.1\esp-idf\tools\idf.py -p COM7 flash monitor
```
