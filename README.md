# ESP32 双 AI 宠物

[中文](README.md) | [English](README.en.md)

本仓库包含两个可以独立配置、编译和烧录的 ESP-IDF 项目：

- `ai-pet/`：AI 宠物“小智”，与用户进行友好、简短的语音对话。
- `anti-pet/`：反 AI 宠物“大神”，主动向小智提问，测试它的记忆、逻辑、事实性和稳定性。

两个角色不绑定硬件。任一项目都可以选择 ESP32-S3-BOX-3、
ESP32-S3-LCD-EV-BOARD-2，或以后新增的开发板。

## 主要功能

- 设备端麦克风采集和扬声器播放。
- 阿里云 DashScope ASR/TTS，可使用 ESP32 内置语音网关，也可切换外部网关。
- DeepSeek、OpenAI 和智谱等聊天模型。
- 多轮对话上下文。
- LVGL 中文界面。
- 设备端 WakeNet 唤醒。
- 串口诊断、远程截图和模拟触摸调试。
- 两台设备之间的语音闭环压力测试。

默认行为：

| 项目 | 角色 | 默认板型 | 默认端口 | 启动会话 |
|---|---|---|---|---|
| `ai-pet` | 小智 | ESP32-S3-BOX-3 | `/dev/ttyACM0` | “你好，小智”或屏幕按钮 |
| `anti-pet` | 大神 | ESP32-S3-LCD-EV-BOARD-2 | `/dev/ttyUSB0` | 屏幕“挑战小智”按钮；首次自动唤醒小智 |

“你好，大神”需要单独训练并集成 WakeNet 模型。在模型可用前，大神使用屏幕按钮
主动发起会话。

## 项目结构

```text
esp32-ai-box/
├── ai-pet/                 # 小智：独立 ESP-IDF 项目
├── anti-pet/               # 大神：独立 ESP-IDF 项目
├── components/             # 两个项目共享的业务、UI、音频和板级组件
├── assets/                 # 公共字体和 SPIFFS 资源
├── cmake/                  # 公共构建配置
├── docs/                   # 详细设计和测试方案
└── tools/                  # 截图、模拟输入、语音和构建辅助工具
```

每个项目拥有自己的：

- `sdkconfig`：本机配置，包含 Wi-Fi 和 API Key，Git 会忽略它。
- `sdkconfig.defaults`：该角色的默认配置。
- `dependencies.lock`：已提交的依赖版本锁。
- `build/` 和 `managed_components/`：独立构建缓存，Git 会忽略它们。

不要在仓库根目录执行 `idf.py`，也不要在两个项目之间复制整个 `sdkconfig`。

## 环境要求

- ESP-IDF 6.0.1（项目也兼容满足组件要求的 ESP-IDF 5.2+ 环境）。
- ESP32-S3 开发板和可访问的串口。
- 首次构建时可访问 Espressif Component Registry。
- Wi-Fi、聊天模型 API Key，以及使用内置语音网关时的 DashScope API Key。

加载本机 ESP-IDF 环境，例如：

```bash
. /home/aladdin/.espressif/v6.0.1/esp-idf/export.sh
```

如果安装路径不同，请使用本机实际的 `export.sh`。

## 配置和编译 ai-pet

### 1. 进入项目

```bash
cd ai-pet
```

### 2. 配置

```bash
idf.py menuconfig
```

至少完成以下配置：

1. `Ai-Box Configuration -> Target board`：选择实际开发板。
2. `Ai-Box Configuration -> WiFi SSID / WiFi Password`：填写网络信息。
3. `Ai-Box Configuration -> Default Chat Provider`：选择聊天服务；然后在
   `Chat Provider Profiles` 填写对应 API Key。
4. `Ai-Box Configuration -> Voice Configuration -> Voice Gateway`：
   - 推荐选择 `Embedded on ESP32`；
   - 填写 DashScope API Key；
   - 需要 PC/服务器网关时再选择 `External HTTP gateway`。
5. `Voice Runtime`：确认设备端 WakeNet 已开启。

保存并退出。配置写入 `ai-pet/sdkconfig`。

### 3. 编译

```bash
idf.py build
```

### 4. 烧录并查看日志

```bash
idf.py -p /dev/ttyACM0 flash monitor
```

### 5. 功能验证

1. 确认屏幕完整显示“小智”界面并连接 Wi-Fi。
2. 确认串口出现 `WakeNet ready`。
3. 说“你好，小智”，或点击屏幕会话按钮。
4. 说一个简短问题。
5. 确认设备完成 ASR、聊天回复和 TTS 播放，并最终回到空闲状态。

## 配置和编译 anti-pet

### 1. 进入项目

```bash
cd anti-pet
```

### 2. 配置

```bash
idf.py menuconfig
```

按照与 `ai-pet` 相同的步骤配置板型、Wi-Fi、聊天 API 和 DashScope API。
默认未启用“你好，大神”WakeNet，因为仓库中尚未包含该自定义模型。

### 3. 编译

```bash
idf.py build
```

### 4. 烧录并查看日志

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

### 5. 功能验证

1. 确认屏幕完整显示“大神”界面并连接 Wi-Fi。
2. 点击“挑战小智”。
3. 首次会话确认大神先说“你好，小智”，然后显示正在等待小智回答。
4. 确认大神的 ASR 识别到小智回复“我在”；只有确认成功后才生成并播报挑战问题。
5. 如果未听到“我在”，确认界面提示重试，并且大神没有直接开始挑战。
6. 确认设备没有重启，并在结束后回到空闲状态。

## 双设备闭环测试

1. 分别烧录 `ai-pet` 和 `anti-pet`。
2. 将两台设备相对放置，避免扬声器紧贴对方麦克风。
3. 同时监控 `/dev/ttyACM0` 和 `/dev/ttyUSB0`。
4. 点击大神的“挑战小智”：首次会话由大神说“你好，小智”，等待小智回复“我在”，
   握手成功后，大神继续监听，确认小智连续静音后才发出第一条挑战。
5. 等小智回答完毕后，再启动大神的下一轮挑战；同一次运行中无需重复首次握手。
6. 大神会自动识别小智的回答并基于上下文继续追问，默认最多自动进行 5 个回复轮次。
7. 检查上下文、误唤醒、ASR/TTS 失败和设备重启。

不要让两个设备无限自动互相触发。测试控制端应限制轮数，并为每轮设置超时。

完整方案见 [PC 与 ESP32 音频闭环测试](docs/pc_esp32_audio_loopback_test_plan.md)。

## 常用命令

在项目目录内：

```bash
idf.py menuconfig       # 修改当前项目配置
idf.py build            # 编译当前项目
idf.py fullclean        # 清理当前项目缓存
idf.py -p PORT flash    # 烧录
idf.py -p PORT monitor  # 查看串口日志
```

也可以从仓库根目录使用：

```bash
tools/build_target.sh ai-pet build
tools/build_target.sh ai-pet flash /dev/ttyACM0
tools/build_target.sh anti-pet build
tools/build_target.sh anti-pet flash /dev/ttyUSB0
```

## Agent 操作规范

自动化 Agent 修改或验证本项目时，应按以下顺序执行：

1. 根据任务选择 `ai-pet/` 或 `anti-pet/`，不要从根目录构建。
2. 不读取、打印或提交 `sdkconfig` 中的 Wi-Fi 密码和 API Key。
3. 不直接修改 `managed_components/`。
4. 公共功能放入 `components/`；角色行为只放入对应项目的 `main/app_role.c`。
5. 板级代码放入 `components/boards/<board>/`，不要用角色判断板型。
6. 修改公共组件后，分别编译两个项目。
7. 修改板型或依赖后先执行 `idf.py fullclean`，再重新编译。
8. 报告实际使用的项目、板型、串口、编译结果和验证结果。

最低交付检查：

```bash
cd ai-pet && idf.py build
cd ../anti-pet && idf.py build
git diff --check
git status --short
```

## 调试

- 串口：使用 `idf.py monitor` 查看启动原因、网络、WakeNet、ASR、LLM 和 TTS 状态。
- 截图：设备联网后运行：

  ```bash
  python tools/screenshot/screenshot.py <DEVICE_IP> --out /tmp/device.png
  ```

- 模拟点击：

  ```bash
  python tools/input/tap.py <DEVICE_IP> tap <X> <Y>
  ```

默认调试端口为截图 `3333`、模拟触摸 `3334`。

## 常见问题

### `idf.py fullclean` 报 managed_components 被修改

`managed_components/` 是自动生成目录。如果改动不需要，备份错误中指出的具体组件
目录后重新执行 `fullclean`。如果改动需要保留，应将它迁移为受版本控制的项目组件，
不能修改 `.component_hash` 或 `CHECKSUMS.json` 绕过检查。

### 修改板型后仍使用旧配置

在对应项目目录执行：

```bash
idf.py fullclean
idf.py menuconfig
idf.py build
```

### 根目录 sdkconfig 是否生效

不生效。实际配置是 `ai-pet/sdkconfig` 和 `anti-pet/sdkconfig`。根目录的
`sdkconfig.defaults.esp32s3*` 是仍需保留的共享默认配置。

### “你好，大神”为什么不能直接配置

WakeNet 使用声学模型，不是字符串匹配。必须先获得并集成相应模型；当前使用屏幕
按钮启动大神。详见 [唤醒词更换与模型集成](docs/wake_word_change_plan.md)。

## 更多文档

- [唤醒词更换与模型集成](docs/wake_word_change_plan.md)
- [PC 与 ESP32 音频闭环测试](docs/pc_esp32_audio_loopback_test_plan.md)
- [语音聊天技术方案](docs/voice-chat-tts-stt-technical-plan.md)
