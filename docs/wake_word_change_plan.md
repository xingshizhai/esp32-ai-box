# ESP32 触发关键词更换方案

## 先明确当前实现

当前固件有两种不同的“唤醒词”机制，不能混为一谈：

1. **本地离线 WakeNet**：真正由 ESP32 麦克风在设备端判断，不依赖网络。当前代码固定加载 `nihaoxiaozhi`，对应“你好小智”模型。
2. **ASR 文本门控**：先把语音送到 ASR，再对识别出的文字做前缀匹配。这种方式理论上可以支持任意 UTF-8 关键词，但当前仓库的 README 中提到的 `ENABLE_VOICE_WAKEUP` / `VOICE_WAKEUP_WORDS` 并没有出现在当前 Kconfig 配置中，不能直接当作已可用的 menuconfig 功能。

因此，想把本地触发词从“你好小智”改成“嗨小智”或“你好盒子”，必须准备对应的 WakeNet 模型；仅修改一个字符串不会改变声学识别模型。

## 方案 A：切换 Espressif 已提供的 WakeNet 模型

适用于“Hi ESP”“小爱同学”“Alexa”等已经存在于 `esp-sr` 模型包中的关键词。

### 步骤

1. 执行 `idf.py menuconfig`。
2. 进入 `Component config → ESP Speech Recognition → WakeNet`。
3. 只选择一个目标 WakeNet 模型，并关闭当前的 `SR_WN_WN9_NIHAOXIAOZHI_TTS`。
4. 在代码中把 `main/app_runtime.c` 的模型过滤字符串从：

   ```c
   esp_srmodel_filter(models, ESP_WN_PREFIX, "nihaoxiaozhi");
   ```

   改为目标模型 ID，例如 `hiesp`、`xiaoaitongxue` 等实际模型名。
5. 同步修改启动日志和 UI 提示中的“你好小智”，避免调试信息与实际模型不一致。
6. 编译、烧录，并在串口确认：

   ```text
   WakeNet ready: phrase=<目标词> model=<实际模型名>
   ```

### 注意事项

- 以 `build/srmodels` 生成的实际模型清单为准，不要凭显示名称猜模型 ID。
- 同时选择多个大模型会增加 `model` 分区和 Flash 占用；建议只保留当前使用的一个。
- WakeNet 词条和 TTS 声音是两套配置，修改唤醒词不会自动修改 TTS voice。

## 方案 B：增加可配置的本地模型 ID 和显示词

这是推荐的固件架构改进，避免每次换词都改 C 源码。

### 新增 menuconfig 配置

建议在 `Voice Runtime` 增加：

```text
LOCAL_WAKEUP_MODEL_ID     string  默认 nihaoxiaozhi
LOCAL_WAKEUP_PHRASE       string  默认 你好小智
```

运行时使用：

```c
esp_srmodel_filter(models, ESP_WN_PREFIX, CONFIG_LOCAL_WAKEUP_MODEL_ID);
```

所有日志和 UI 文案使用 `CONFIG_LOCAL_WAKEUP_PHRASE`。启动时检查模型是否存在；不存在时显示明确错误，而不是静默退出唤醒任务。

### 配置示例

```ini
CONFIG_LOCAL_WAKEUP_MODEL_ID="nihaoxiaozhi"
CONFIG_LOCAL_WAKEUP_PHRASE="你好小智"
```

切换到已安装模型时只需修改这两项，然后执行：

```bash
idf.py reconfigure
idf.py build
idf.py -p /dev/ttyACM0 flash
```

## 方案 C：自定义任意关键词

适用于“小智”“你好盒子”“小伙伴”等 `esp-sr` 没有现成模型的词。

### 流程

1. 选择目标词，并固定普通话发音、音节数和允许的变体。
2. 按 Espressif ESP-SR/WakeNet 的模型训练或定制流程准备正样本、噪声样本、不同说话人和不同距离数据。
3. 生成与当前 ESP32-S3/ESP-SR 版本兼容的 WakeNet 模型。
4. 将模型放入 `model` 分区构建输入，并为它定义唯一模型 ID。
5. 使用方案 B 的 `LOCAL_WAKEUP_MODEL_ID` / `LOCAL_WAKEUP_PHRASE` 选择它。
6. 在安静、办公室、播放音乐、多人说话四种环境分别测试误唤醒和漏唤醒。

不能用普通 ASR 关键词匹配替代真正的本地唤醒：ASR 方案依赖网络、延迟高，且无法在离线状态可靠工作。

## ASR 文本门控方案（可选补充）

如果产品接受“联网后才判断唤醒词”，可以增加正式 Kconfig：

```text
ENABLE_VOICE_WAKEUP=y
VOICE_WAKEUP_WORDS="小智,你好小智,你好盒子"
VOICE_WAKEUP_WINDOW_MS=8000
```

实现要求：

- 在 ASR 最终文本上做 UTF-8 前缀匹配。
- 支持中文标点和空格分隔。
- 只有匹配唤醒词后才提交 LLM。
- 唤醒词后带有问题时直接处理，例如“你好小智，讲个笑话”。
- 只有单独唤醒词时进入短暂的二次聆听窗口。
- 区分本地 WakeNet 和 ASR 门控的日志标签，便于排查延迟和误触发。

## 更换后的验收测试

### 功能

- 连续说目标词 20 次，统计触发次数。
- 说旧词 20 次，确认旧词不再触发（若模型确实不同）。
- 说目标词后立即说问题，确认进入完整 ASR→LLM→TTS 链路。
- 只说目标词，确认进入二次聆听窗口。
- 未联网时说目标词，确认本地 WakeNet 仍能触发。

### 稳定性

- 连续运行 30 分钟，观察是否重复触发、任务泄漏或重启。
- TTS 播放时说目标词，确认 barge-in 行为符合配置。
- 保存串口中的 `WakeNet ready`、`WakeNet detected`、状态转换和复位原因。

### 通过标准

```text
target_wake_recall >= 95%
old_wake_false_trigger = 0  （若使用完全不同模型）
offline_trigger = PASS
device_reboot = NO
```

## 当前项目的建议落地顺序

1. 先保留当前稳定的“你好小智”模型。
2. 把硬编码模型 ID 和提示词抽成 `LOCAL_WAKEUP_MODEL_ID` / `LOCAL_WAKEUP_PHRASE`。
3. 再选择 `esp-sr` 已有模型做切换验证。
4. 产品最终需要“任意中文词”时，再进入自定义 WakeNet 模型训练，不要只改字符串。
