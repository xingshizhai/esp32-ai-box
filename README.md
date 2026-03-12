# AI Chat Demo for ESP32-S3-BOX-3

一个模块化、易扩展的AI聊天机器人项目，支持多种AI模型切换，专门针对中国大陆使用优化。

## 功能特点

- **多模型支持**：OpenAI、智谱AI(GLM)、DeepSeek，易于扩展其他模型
- **语音交互**：完整的语音识别、AI对话、语音播报功能
- **对话记忆**：支持上下文理解的对话历史管理
- **中国大陆友好**：优先使用国内AI服务，支持代理配置
- **模块化设计**：清晰的架构分层，方便维护和扩展
- **调试功能**：内置麦克风和音频播放调试界面

## 硬件要求

- ESP32-S3-BOX-3开发板
- USB-C数据线

## 项目结构

```
.
│   ├── main.c              # 主程序入口
│   ├── components/         # 功能模块
│   │   ├── ai_service/     # AI服务抽象层
│   │   │   ├── providers/  # 各个AI提供商实现
│   │   │   │   ├── openai.c    # OpenAI实现
│   │   │   │   ├── zhipu.c      # 智谱AI实现
│   │   │   │   └── deepseek.c  # DeepSeek实现
│   │   ├── conversation/   # 对话历史管理
│   │   ├── config/         # 配置管理(NVS)
│   │   ├── network/        # 网络连接管理
│   │   ├── ui/             # LVGL界面控制
│   │   └── audio/          # 音频处理
│   ├── CMakeLists.txt
│   └── Kconfig.projbuild
├── spiffs/                 # 音频资源文件
├── partitions.csv
└── README.md
```

## 快速开始

### 1. 环境准备

```bash
# 设置ESP-IDF环境
. $HOME/esp/esp-idf/export.sh
```

### 2. 配置项目

```bash
idf.py menuconfig
```

在menuconfig中配置：
- 选择AI提供商（推荐DeepSeek，国内可用）
- 设置API密钥
- 配置WiFi信息

### 3. 编译和烧录

```bash
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

## 配置说明

### AI模型配置

可以通过 `idf.py menuconfig` 配置：

- **AI Provider**: 选择OpenAI、智谱AI或DeepSeek
- **API Key**: 对应服务的API密钥
- **Base URL**: API基础URL
- **Model Name**: 模型名称
- **Max Tokens**: 最大响应token数
- **Temperature**: 温度参数(0.0-2.0)

### 默认配置

项目默认配置为DeepSeek，适合中国大陆使用：

```ini
AI_PROVIDER: DeepSeek
BASE_URL: https://api.deepseek.com/v1/chat/completions
MODEL: deepseek-chat
```

## 调试功能

项目包含一个完整的调试界面，用于测试麦克风和音频播放功能。

### 进入调试界面

1. 启动设备后，在主界面点击"Debug"按钮
2. 进入调试面板后，可以看到以下功能：
   - 麦克风实时音量显示
   - 录音/播放录音测试
   - 音频播放测试

### 麦克风测试

1. 进入调试界面，点击"Record"按钮开始录音
2. 对着麦克风说话，观察实时音量条
3. 再次点击"Record"停止录音
4. 点击"Play"播放录制的音频

### 音频播放测试

1. 点击"Test Audio"按钮
2. 系统会播放测试音频（440Hz正弦波，持续1秒）
3. 观察播放状态变化

### 调试功能API

```c
// 开始/停止麦克风监控
audio_debug_start_monitor(void);
audio_debug_stop_monitor(void);

// 注册音量回调
audio_register_mic_level_callback(callback);

// 录制样本
audio_debug_record_sample(&data, &len);

// 播放测试音频
audio_debug_play_test_audio();
```

## 扩展新的AI模型

### 1. 创建provider实现

在 `main/components/ai_service/providers/` 下创建新文件：

```c
// new_provider.c
#include "ai_service.h"

static esp_err_t new_provider_init(ai_service_t *service, const ai_config_t *config);
static esp_err_t new_provider_chat(ai_service_t *service, const char *user_message, ai_response_t *response);

ai_service_t* new_provider_service_create(void) {
    ai_service_t *service = calloc(1, sizeof(ai_service_t));
    service->init = new_provider_init;
    service->chat = new_provider_chat;
    return service;
}
```

### 2. 注册provider

在 `ai_service.c` 中添加：

```c
ai_service_t* create_new_provider_service(void) {
    return new_provider_service_create();
}

ai_service_t* ai_service_create(ai_provider_type_t provider) {
    switch (provider) {
        case AI_PROVIDER_NEW:
            return create_new_provider_service();
        // ...
    }
}
```

### 3. 更新配置

在 `config.h` 和 `Kconfig.projbuild` 中添加新provider选项。

## API密钥获取

### DeepSeek
- 访问: https://platform.deepseek.com/
- 注册账号并获取API Key
- 免费额度充足

### 智谱AI
- 访问: https://open.bigmodel.cn/
- 注册并创建API Key
- 使用GLM-4模型

### OpenAI
- 访问: https://platform.openai.com/
- 需要代理才能在中国大陆使用

## 故障排除

### 编译错误
```bash
# 清理构建缓存
idf.py fullclean
idf.py build
```

### WiFi连接失败
- 检查SSID和密码配置
- 确认ESP32-S3-BOX-3的WiFi工作正常

### AI请求失败
- 检查API密钥是否正确
- 确认网络连接正常
- 检查API URL配置

## 技术架构

### 核心模块

1. **AI服务层** (ai_service)
   - 统一的AI服务接口
   - 支持多provider切换
   - HTTP请求和JSON解析

2. **对话管理** (conversation)
   - 对话历史存储
   - 上下文管理
   - 消息队列

3. **配置管理** (config)
   - NVS持久化存储
   - 运行时配置
   - 工厂重置

4. **网络管理** (network)
   - WiFi连接
   - 状态回调
   - 重试机制

5. **UI控制** (ui)
   - LVGL界面
   - 面板切换
   - 消息显示

6. **音频处理** (audio)
   - 语音识别
   - TTS播放
   - 音量控制

## 许可证

CC0-1.0

## 贡献

欢迎提交Issue和Pull Request！
