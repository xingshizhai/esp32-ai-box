# ESP32 双 AI 宠物

同一仓库中的两个独立 ESP-IDF 项目：AI 宠物“小智”和用于主动压力测试的
反 AI 宠物“十神”。两者共享硬件、语音、AI、UI 和调试组件，但拥有独立的
`sdkconfig`、构建缓存、固件入口和产品行为。

## 功能特点

- **多模型支持**：OpenAI、智谱AI(GLM)、DeepSeek、Kimi、MiniMax、OpenRouter，易于扩展其他模型
- **语音交互**：完整的语音识别、AI对话、语音播报功能
- **对话记忆**：支持上下文理解的对话历史管理
- **中国大陆友好**：优先使用国内AI服务，支持代理配置
- **模块化设计**：清晰的架构分层，方便维护和扩展
- **调试功能**：内置麦克风和音频播放调试界面

## 支持的硬件

- ESP32-S3-BOX-3
- ESP32-S3-LCD-EV-BOARD-2
- 可通过 `components/boards/custom` 扩展其他板型

角色不由板型决定：两个项目都可以在 menuconfig 中选择任意受支持板型。

## 项目结构

```text
.
├── ai-pet/                 # 独立 ESP-IDF 项目：小智
│   ├── CMakeLists.txt
│   ├── main/app_role.c
│   ├── sdkconfig.defaults
│   └── partitions.csv
├── anti-pet/               # 独立 ESP-IDF 项目：十神
│   ├── CMakeLists.txt
│   ├── main/app_role.c
│   ├── sdkconfig.defaults
│   └── partitions.csv
├── components/             # 两个项目共同复用
│   ├── app_core/           # 通用入口、语音运行时、串口调试
│   ├── boards/             # 板级显示、触摸与音频接线
│   ├── ai_service/
│   ├── audio/
│   ├── conversation/
│   ├── ui/
│   └── voice_chat/
├── assets/                 # 公共 SPIFFS 字体/资源
├── cmake/                  # 公共 ESP-IDF 项目引导
└── tools/
```

## 快速开始

### 1. 环境准备

```bash
# 设置ESP-IDF环境
. $HOME/esp/esp-idf/export.sh
```

### 2. 配置并编译 AI 宠物

```bash
cd ai-pet
idf.py menuconfig
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

在menuconfig中配置：
- 选择AI提供商（推荐DeepSeek，国内可用）
- 设置API密钥
- 配置WiFi信息

### 3. 配置并编译反 AI 宠物

```bash
cd ../anti-pet
idf.py menuconfig
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

默认部署是 `ai-pet` 使用 BOX-3、`anti-pet` 使用 LCD-EV，但这不是代码约束。
进入任一项目执行 `idf.py menuconfig`，都可以更换目标板型。切换板型后应执行
`idf.py fullclean` 再构建。

也可以从仓库根目录使用快捷脚本：

```bash
tools/build_target.sh ai-pet build
tools/build_target.sh ai-pet flash /dev/ttyACM0
tools/build_target.sh anti-pet build
tools/build_target.sh anti-pet flash /dev/ttyUSB0
```

两个目录各自保存被 Git 忽略的 `sdkconfig`，因此 Wi-Fi、API Key 和板型配置互不
覆盖；各自的 `dependencies.lock` 已纳入版本控制，并各自生成被 Git 忽略的
`build/` 和 `managed_components/`。切换角色或板型不会污染另一个项目的缓存。
首次构建需要联网下载锁定组件，后续构建可复用各自缓存。不要再从仓库根目录运行
`idf.py`；根目录不是 ESP-IDF 项目。

配置迁移时不要复制整个旧 `sdkconfig` 覆盖另一项目。应分别进入项目运行
`menuconfig`，或只迁移 Wi-Fi/API 等明确配置；密钥文件均保持 Git 忽略。

根目录不应再保留运行配置 `sdkconfig`、`sdkconfig.old` 或旧的
`dependencies.lock`；真正生效的是 `ai-pet/sdkconfig` 和 `anti-pet/sdkconfig`。
根目录的 `sdkconfig.defaults.esp32s3*` 仍需保留，它们是两个项目共享的芯片和板级
默认配置，不包含本机密钥。

## 配置说明

### AI模型配置

可以通过 `idf.py menuconfig` 配置：

- **AI Provider**: 选择OpenAI、智谱AI、DeepSeek、Kimi、MiniMax或OpenRouter
- **API Key**: 对应服务的API密钥
- **Base URL**: API基础URL
- **Model Name**: 模型名称
- **Max Tokens**: 最大响应token数
- **Temperature**: 温度参数(0.0-2.0)
- **OpenRouter Headers(可选)**: `HTTP-Referer` 与 `X-Title`，用于 OpenRouter 路由统计与兼容

提示：建议在 `menuconfig` 或设备 NVS 中设置密钥，不要将真实 API Key 写入仓库文件。

### 默认配置

项目默认配置为DeepSeek，适合中国大陆使用：

```ini
AI_PROVIDER: DeepSeek
BASE_URL: https://api.deepseek.com/v1/chat/completions
MODEL: deepseek-chat
```

### 语音唤醒（可选）

触发词更换和模型选择的完整说明见 [ESP32 触发关键词更换方案](docs/wake_word_change_plan.md)。
`ai-pet` 默认启用设备端“你好，小智”WakeNet；`anti-pet` 在真实“你好，十神”
模型交付前默认关闭 WakeNet，使用屏幕按钮启动主动挑战。仅修改显示字符串不会
改变声学模型。

#### 生成新的 WakeNet 唤醒词模型

WakeNet 的唤醒词是神经网络模型，不是配置字符串。修改界面文字、
`wake_phrase` 或 `esp_srmodel_filter()` 的过滤词，都不会让设备学会一个新词。
以“你好，十神”为例，必须先得到包含该词的 WakeNet 模型。

官方目前提供两条路径：

1. **开源项目 / TTS 训练路径**：在 ESP-SR 的唤醒词征集 issue 提交词语、
   项目链接和用途，或按该页最新的 TTS Pipeline 流程申请生成模型。官方说明
   TTS Pipeline V3 已支持中文、英文、日文和法文。社区模型适合原型验证，仍需
   在真实设备上测误唤醒率和召回率。
2. **量产定制路径**：联系乐鑫销售定制。官方文档说明可由客户提供语料，或由
   乐鑫提供语料并训练；高质量真人语料方案通常需要收费和数周时间。量产产品
   应优先采用这条路径并完成噪声、距离、男女声和儿童声测试。

官方入口：

- [ESP-SR WakeNet 文档](https://docs.espressif.com/projects/esp-sr/zh_CN/latest/esp32s3/wake_word_engine/README.html)
- [乐鑫语音唤醒方案客户定制流程](https://docs.espressif.com/projects/esp-sr/zh_CN/latest/esp32s3/wake_word_engine/ESP_Wake_Words_Customization.html)
- [TTS 唤醒词训练/社区申请（ESP-SR issue #88）](https://github.com/espressif/esp-sr/issues/88)

拿到模型后的集成步骤如下。假设官方交付的模型目录名为
`wn9_nihaoshishen_tts3`。每个产品项目维护独立依赖目录，以下操作应在
`anti-pet/managed_components` 中完成：

```text
anti-pet/managed_components/espressif__esp-sr/model/wakenet_model/
└── wn9_nihaoshishen_tts3/
    ├── ...模型文件...
    └── ...模型元数据...
```

1. 将**完整交付目录**放到上述 `wakenet_model` 目录。不要改模型内部文件名。
2. 在 `anti-pet/managed_components/espressif__esp-sr/Kconfig.projbuild` 的 WakeNet 模型
   列表中增加布尔项，例如：

   ```kconfig
   config SR_WN_WN9_NIHAOSHISHEN_TTS3
       bool "你好十神 (wn9_nihaoshishen_tts3)"
       default n
   ```

   布尔默认值只能写 `y` 或 `n`，不能写 `True`、`False`、`0` 或 `1`。
3. 在十神目标的 sdkconfig/defaults 中启用它：

   ```ini
   CONFIG_SR_WN_WN9_NIHAOSHISHEN_TTS3=y
   ```

4. 让角色的 `wake_model_filter` 与模型目录的可识别部分一致。本项目十神角色已
   预留 `nihaoshishen`；运行时会从 `model` 分区查找对应模型。找不到时会在
   串口明确输出 `WakeNet unavailable`，屏幕按钮仍可启动十神主动发言。
5. 模型分区由 ESP-SR 构建脚本根据 Kconfig 重新生成，因此必须执行一次干净构建
   并烧录全部镜像（不能只烧 app）：

   ```bash
   cd anti-pet
   idf.py fullclean
   idf.py build
   idf.py -p /dev/ttyUSB0 flash
   ```

6. 从串口确认日志包含 `WakeNet ready`、目标短语和目标模型名，再用真人录音做
   至少三类回归：安静环境召回、扬声器回声期间不误触、数小时背景音误唤醒。

注意：`managed_components` 通常由依赖管理器重新生成，直接修改其中的 Kconfig
可能在更新依赖后丢失。产品化时应把定制模型做成受版本控制的私有组件或补丁，
并固定 `espressif/esp-sr` 版本；不要把有授权限制的模型公开提交到仓库。

### 语音识别 / 语音合成（千问 / 百炼）

设备侧语音链路通过 **Voice Gateway** 统一对接 ASR/TTS（见 `components/voice_chat/`）。云端推荐使用千问/百炼语音能力，官方文档：

- 实时语音识别（ASR）：https://platform.qianwenai.com/docs/developer-guides/speech/asr-realtime
- 实时语音合成（TTS）：https://platform.qianwenai.com/docs/developer-guides/speech/realtime-streaming

本机联调（不烧录设备）可先验证云端 Key 与协议：

```bash
# 1) 配置 Key（勿提交）：tools/voice/.env.local
#    DASHSCOPE_API_KEY=sk-xxxx

# 2) 可选：直接测百炼 ASR/TTS
python tools/voice/test_asr_tts.py

# 3) 启动本机 Voice Gateway（给 ESP32 用）
python tools/voice/gateway.py --host 0.0.0.0 --port 8787

# 4) 另开终端，对本机网关做 HTTP 冒烟测试
python tools/voice/test_gateway.py http://127.0.0.1:8787
```

ESP32 侧在 `menuconfig -> Voice Configuration -> Voice Gateway` 设置：

- `ENABLE_VOICE_GATEWAY=y`
- `DEFAULT_VOICE_GATEWAY_URL=http://<电脑局域网IP>:8787`  
  例如电脑 IP 为 `192.168.1.10` 时写成 `http://192.168.1.10:8787`

网关协议与固件 `voice_gateway_client.c` 对齐：`/v1/stt/start`、`/v1/stt/chunk`、`/v1/stt/stop`、`/v1/tts`。

本地离线唤醒说明：

- 可在 `menuconfig -> Ai-Box Configuration -> Voice Configuration -> Voice Runtime` 开启 `ENABLE_LOCAL_OFFLINE_WAKEUP`。
- 该模式下，设备端持续采集麦克风并运行 ESP-SR WakeNet，不依赖网络判断唤醒。
- 唤醒短语和模型过滤词来自当前产品的 `main/app_role.c`，不是板级配置。
- `LOCAL_WAKEUP_COOLDOWN_MS` 设置两次有效触发之间的冷却时间。
- 触发后才进入现有语音轮次（此时 STT/LLM 仍按你当前网关链路执行）。

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

### 串口快速切换 Provider

在 `idf.py monitor` 中可直接输入以下命令（回车执行）：

```text
help
provider list
provider show
provider set deepseek
provider preset deepseek
provider preset openrouter
provider set kimi
provider set openrouter
provider key set sk-xxxx
provider base set https://api.deepseek.com/v1/chat/completions
provider model set deepseek-chat
provider referer set https://your-app.example.com
provider title set esp32-ai-box
provider test
provider test Reply with hello
```

说明：
- `provider set <name>` 会写入 NVS 并立即重建 AI service。
- `provider preset <name>` 会应用该 provider 的推荐 base/model，并激活该 provider。
- 如果目标 provider 初始化失败，会自动回滚到之前的 provider。
- `provider key/base/model set <value>` 会更新当前 provider 配置并立即重建 AI service。
- `provider referer/title set <value>` 用于更新 OpenRouter 可选请求头；当前 provider 是 OpenRouter 时会立即重建。
- `provider test [prompt]` 会直接发起一次聊天请求并输出响应或错误信息。

## UI 截图闭环调试

当前工程已经内置了两个调试服务：

- 截图服务：TCP `3333`
- 合成触摸服务：TCP `3334`

它们的用途是让你在电脑上自动完成“看图 -> 分析 -> 改代码 -> 再看图”的闭环，不需要盯着实体屏幕。

### 使用方法

1. 烧录并启动设备，确保设备已连上 Wi-Fi。
2. 从串口日志里找到设备 IP。
3. 在电脑上执行截图脚本，保存当前界面：

```bash
python tools/screenshot/screenshot.py <device-ip> --out tools/screenshot/screenshots/current.png
```

4. 打开生成的 PNG，分析布局、文字、颜色是否正确。
5. 如需验证点击路径，执行合成触摸脚本：

```bash
python tools/input/tap.py <device-ip> tap <x> <y>
```

6. 再截一张图，确认页面切换或状态变化。

### 脚本依赖

这些脚本使用 Python，依赖：

- `numpy`
- `Pillow`

可以直接安装：

```bash
pip install numpy Pillow
```

### 说明

- 截图服务返回的是当前 LVGL 活动屏的 RGB565 数据。
- 合成触摸服务把 `tap/down/move/up` 转成 LVGL 指针事件。
- 这套闭环适合我继续帮你自动分析界面，再反向修改 `ui.c`。

## 扩展新的AI模型

### 1. 创建provider实现

在 `components/ai_service/providers/` 下创建新文件：

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
# 先进入 ai-pet/ 或 anti-pet/
idf.py fullclean
idf.py build
```

`fullclean` 会清除当前项目的 `build/` 和 `managed_components/`，但不会影响另一个
项目的缓存，也不会删除当前项目的 `sdkconfig`。

### fullclean 报 managed_components 被修改

`managed_components/` 由 ESP-IDF Component Manager 自动维护，不应直接修改。
如果出现下面的错误：

```text
ERROR: Some components in the "managed_components" directory were modified
```

先根据错误中的组件名确认它是否仍在当前项目的 `dependencies.lock` 中：

```bash
rg 'espressif/esp-box' dependencies.lock
```

- 如果是未被依赖锁引用的旧组件，将该**具体组件目录**移动到 `/tmp` 备份，再运行
  `idf.py fullclean`。例如旧 `espressif__esp-box` 已被当前
  `espressif__esp-box-3` 替代时：

  ```bash
  mv managed_components/espressif__esp-box /tmp/espressif__esp-box.backup
  idf.py fullclean
  idf.py build
  ```

- 如果改动仍然需要，不要丢弃。将组件迁移为仓库内受版本控制的公共组件或私有
  组件，并通过 `idf_component.yml` 的 `override_path` 显式引用，再重新构建。

不要为了绕过校验而修改 `.component_hash` 或 `CHECKSUMS.json`。这会隐藏本地改动，
导致依赖无法可靠复现。

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
