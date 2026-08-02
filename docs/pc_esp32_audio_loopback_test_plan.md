# PC ↔ ESP32 音频闭环测试方案

## 目标

验证 AI 宠物单机以及两台角色设备之间的完整语音链路：

`PC 播放语音 → ESP32 麦克风 → 阿里 ASR → DeepSeek → 阿里 TTS → ESP32 扬声器 → PC USB 麦克风回采`

测试必须覆盖单轮、多轮上下文、长回复、异常恢复和设备稳定性。

## 双设备部署

推荐但不强制的实验室部署如下；角色并不绑定板型：

| 产品项目 | 默认角色 | 示例板型 | 串口 | 启动方式 |
|---|---|---|---|---|
| `ai-pet/` | 小智 | BOX-3 | `/dev/ttyACM0` | “你好，小智”或屏幕按钮 |
| `anti-pet/` | 十神 | LCD-EV | `/dev/ttyUSB0` | 屏幕“挑战小智”按钮 |

两个项目必须分别配置、编译和烧录。不要共用 `sdkconfig`、`build/` 或
`managed_components/`：

```bash
cd ai-pet
idf.py build
idf.py -p /dev/ttyACM0 flash

cd ../anti-pet
idf.py build
idf.py -p /dev/ttyUSB0 flash
```

十神按钮会先让 LLM 生成一个短促的压力测试问题，再通过自身 TTS 扬声器主动
说给小智听。现阶段十神不依赖尚未交付的“你好，十神”WakeNet 模型。

## 设备与固定参数

- ESP32 地址：以设备串口或网络扫描结果为准，例如 `192.168.1.166`。
- 串口：`/dev/ttyACM0`，115200 baud。
- ESP32 调试输入：TCP `3334`，用于点击“开始对话”。
- ESP32 截图：TCP `3333`，用于确认界面状态。
- PC 音频输入：优先使用 USB PCM2902 的 ALSA capture 设备，例如 `hw:1,0`。
- PC 音频输出：使用实际可用的 ALSA/PulseAudio 输出设备；测试前执行 `aplay -l` 确认，不假设 USB 声卡同时提供 playback。
- 语音格式：单声道 PCM，ESP32 内部 16 kHz/16 bit；PC 端可用 `spd-say` 播放中文测试语句。

## 测试前检查

```bash
lsusb
arecord -l
aplay -l
python tools/screenshot/screenshot.py <ESP32_IP> --out /tmp/esp32_before.png
```

确认：

1. ESP32 主界面显示已联网、处于待机。
2. `VOICE_GATEWAY_MODE_EMBEDDED=y`，DashScope Key 已配置。
3. `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN` 至少为 `16384`。
4. TTS 超时建议为 `60000 ms`，避免长回复被本地提前终止。
5. PC 扬声器与 ESP32 麦克风距离适中，避免削波；PC USB 麦克风放在 ESP32 扬声器附近但不要贴近。

## 单轮自动化流程

### 1. 启动串口采集

```bash
stty -F /dev/ttyACM0 115200 cs8 -cstopb -parenb -ixon -ixoff raw -echo
timeout 70s stdbuf -oL cat /dev/ttyACM0 > /tmp/esp32_round.log &
SERIAL_PID=$!
```

串口采集必须放到后台，触发和播放完成后再执行 `wait "$SERIAL_PID"`；如果只想实时观察，可同时使用 `tail -f /tmp/esp32_round.log`。

### 2. 触发录音并播放 PC 语音

```bash
python tools/input/tap.py <ESP32_IP> tap 160 213
sleep 2
spd-say -w -l zh-CN -r -25 '你好小智，请记住数字四十二'
```

### 3. 同步回采 ESP32 播报

在预计 TTS 开始前启动 PC USB 麦克风录音：

```bash
arecord -D hw:1,0 -f S16_LE -r 44100 -c 1 -d 45 /tmp/esp32_tts_round.wav
```

`arecord` 如果提示硬件只支持 44100 Hz，应接受实际采样率，或改用 `plughw:1,0` 做重采样。

## 多轮会话用例

每一轮都等待上一轮回到 `IDLE` 或界面显示“开始对话”，再点击开始按钮。不要在 TTS 播放期间再次点击，除非专门测试打断。

| 轮次 | PC 播放内容 | 验证点 |
|---|---|---|
| 1 | “你好小智，请记住数字四十二” | 唤醒、ASR、首条上下文写入 |
| 2 | “我刚才让你记住的数字是多少” | DeepSeek 是否保留上一轮上下文 |
| 3 | “把这个数字乘以二，再简短回答” | 多轮推理、较短 TTS |
| 4 | “请用一句话介绍 ESP32” | 主题切换、正常收尾 |
| 5 | “请详细解释刚才的内容” | 长回复、TTS 超时和大音频缓冲 |

每轮记录：ASR 最终文本、LLM 响应长度、TTS PCM 字节数、总耗时、最终状态。

## 十神 ↔ 小智双机闭环

将两台设备相对摆放，扬声器到对方麦克风建议先保持 20–40 cm。测试时同时采集
两个串口，并用时间戳区分 `anti_pet` 与 `ai_pet`：

1. 点击十神的“挑战小智”，确认它生成问题并完成 TTS。
2. 小智检测“你好，小智”后进入 ASR；若十神生成的问题未带唤醒词，则测试控制器
   应先播放唤醒短语，或用小智屏幕按钮进入聆听后再启动十神。
3. 确认小智完成 ASR → LLM → TTS，十神可通过屏幕按钮发起下一轮质疑。
4. 连续进行至少 5 轮，检查上下文一致性、事实错误、回声误触发和双方是否重启。
5. 每轮保存双方串口日志、前后截图和 PC 麦克风回采 WAV。

闭环控制器应等待当前说话方回到 `IDLE` 后才触发下一方，设置最大轮数和单轮超时，
防止两台设备无限互相唤醒。压力测试内容可以尖锐，但不得引导现实伤害或违法行为。

## 串口验收条件

成功条件：

- ASR 出现 `task-started`、`result-generated`、`task-finished`。
- 出现 `STT trace ok`，且 `stage` 不是 `stt_empty`。
- DeepSeek 出现 `AI response`。
- TTS 出现 `task-started`、多个 `result-generated`、`task-finished`。
- 出现 `Queued ... bytes`、`Playback OK`、`Audio playback completed`。
- 每轮最终回到 `IDLE`，没有 `ESP_ERR_TIMEOUT`、`ESP_ERR_HTTP_CONNECT`、Guru Meditation 或重启启动日志。

失败条件：

- ASR 空文本或上传中断。
- TTS 在 `task-finished` 前超时。
- 出现 `MBEDTLS_ERR_SSL_INVALID_RECORD (-0x7200)`。
- 播放期间设备重启、看门狗复位或内存不足。
- 第二轮回答不包含第一轮设定的数字 `42`。

## PC 回采分析

回采文件不能只检查文件存在，还要计算有效声音比例：

```bash
python -c 'import wave,struct,math; p="/tmp/esp32_tts_round.wav"; w=wave.open(p); b=w.readframes(w.getnframes()); x=struct.unpack("<%dh"%(len(b)//2),b); rms=math.sqrt(sum(v*v for v in x)/len(x)); peak=max(abs(v) for v in x); active=sum(abs(v)>500 for v in x)/len(x); print(f"rate={w.getframerate()} frames={len(x)} rms={rms:.1f} peak={peak} active={active:.1%}")'
```

建议判定：`peak > 2000` 且 `active > 1%`。如果文件 RMS 接近 0，优先检查 PC 播放设备、ESP32 音量和麦克风摆位。

## 稳定性与恢复测试

完成多轮后追加：

1. 连续 5 轮短句会话，确认堆内存没有持续下降或重启。
2. 一轮长回复后立即进行短句，确认 WebSocket 和音频缓冲已释放。
3. 在 TTS 播放期间点击开始对话，确认 barge-in 行为符合配置。
4. 暂时断开 Wi-Fi，再恢复网络，确认错误状态能回到 `IDLE`。
5. 保存串口日志、截图和 WAV 回采文件，按日期放入 `/tmp/esp32-loopback/<timestamp>/`。

## 最终报告格式

每轮输出：

```text
round=2
stt_text="..."
context_check=PASS/FAIL
tts_bytes=...
audio_rms=...
audio_peak=...
active_ratio=...
elapsed_ms=...
device_reboot=YES/NO
result=PASS/FAIL
```
