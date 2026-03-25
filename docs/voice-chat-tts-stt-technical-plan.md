# ESP32 AI Box 语音聊天 TTS/STT 技术方案（中国大陆可用）

## 1. 文档目标

本方案用于指导当前工程落地“语音输入 -> 文本理解 -> 语音回复”的完整闭环，重点设计 STT（语音识别）和 TTS（语音合成）能力，要求在中国大陆网络环境可稳定使用，并适配当前 ESP-IDF 工程架构。

目标结果：
- 支持中文普通话实时语音聊天。
- 优先使用中国大陆可用的云服务。
- 在现有代码基础上最小改动接入，逐步演进到流式低延迟方案。

## 2. 当前工程现状

### 2.1 已有能力

- 已有 AI 服务抽象，包含 chat/stt/tts 接口定义：
  - `main/components/ai_service/ai_service.h`
  - `main/components/ai_service/ai_service.c`
- 已有对话管理与 UI 基础流程：
  - `main/components/conversation`
  - `main/components/ui`
  - `main/main.c`
- 已有音频采集与播放能力（ES7210 + ES8311 + I2S）：
  - `main/components/audio/audio.c`
  - `main/components/audio/audio.h`
- 音频基础格式已稳定在 `16kHz / 16bit / PCM` 路径。

### 2.2 当前缺口

- 各 provider 的 STT/TTS 实现为空（仅 chat 可用）。
- 主流程中 `audio_start_stt` 仅注册回调，未形成真实云端语音识别链路。
- 没有语音网关层，设备直连厂商 API 在鉴权、协议适配、切换容灾方面成本较高。
- 语音会话状态机尚未完整定义（聆听、识别中、思考中、播报中、可打断）。

## 3. 设计原则

- 中国大陆可用优先：避免依赖海外网络可达性。
- 流式优先：优先保证首包时延和交互体验。
- 解耦优先：聊天模型、STT、TTS 独立配置，避免强绑定一个厂商。
- 网关优先：ESP32 仅对接统一语音网关，网关对接多厂商。
- 渐进交付：先实现可用，再优化延迟、自然度、打断能力。

## 4. 技术选型建议

### 4.1 STT 选型

主选：火山引擎流式语音识别（中文场景成熟）
备选：阿里云实时语音识别

能力要求：
- 支持 WebSocket 流式识别。
- 支持中间结果和最终结果。
- 支持中文标点、数字规范化。
- 支持断句和端点检测。

### 4.2 TTS 选型

主选：火山引擎流式语音合成
备选：阿里云流式语音合成（CosyVoice 系）

能力要求：
- 支持中文自然音色。
- 支持流式返回音频数据。
- 优先返回 PCM 16k mono，减少设备端解码成本。

### 4.3 对话模型

沿用现有 DeepSeek / 智谱对话链路，不与 STT/TTS 强耦合。

建议目标架构：
- chat_provider: DeepSeek 或 智谱
- stt_provider: 火山或阿里
- tts_provider: 火山或阿里

## 5. 总体架构

### 5.1 分层

- 设备层（ESP32）
  - 麦克风采集、VAD、音频分片发送
  - 文本上屏、状态机、音频播放
- 网关层（建议新增）
  - 厂商鉴权签名
  - STT/TTS 协议统一
  - 主备切换、限流、审计日志
- 云服务层
  - STT 服务
  - LLM 服务（现有）
  - TTS 服务

### 5.2 时序（单轮）

1. 用户按键说话或唤醒后说话。
2. 设备采集 PCM 分片推送到 STT。
3. 收到 STT final 文本后提交给 LLM。
4. 收到 LLM 文本后发送给 TTS。
5. TTS 音频流返回，设备边收边播。
6. 播报完成，回到待机状态。

## 6. 音频与协议规范

### 6.1 设备音频标准

- 采样率：16000
- 位深：16bit
- 声道：mono（上传时转单声道）
- 编码：PCM S16LE
- STT 分片：20ms（640 bytes）

### 6.2 STT 协议抽象（设备 <-> 网关）

控制消息（JSON 文本帧）：
- `start`: 启动会话，携带 `session_id`, `sample_rate`, `format`
- `stop`: 主动结束输入
- `cancel`: 用户打断

音频消息（二进制帧）：
- 原始 PCM 分片，不做 Base64

结果消息（JSON 文本帧）：
- `partial`: 中间识别
- `final`: 最终识别
- `error`: 错误码与描述

### 6.3 TTS 协议抽象（设备 <-> 网关）

请求：
- `session_id`
- `text`
- `voice`
- `sample_rate`
- `stream=true`

返回：
- 控制帧：`start`, `end`, `error`
- 音频帧：PCM 二进制分片

## 7. 软件模块设计

### 7.1 现有模块改造

1) `main/components/config`
- 拆分配置项：`chat_provider`, `stt_provider`, `tts_provider`
- 新增网关配置：`voice_gateway_url`, `voice_gateway_token`
- 新增语音参数：`vad_silence_ms`, `stt_timeout_ms`, `tts_timeout_ms`, `enable_barge_in`

2) `main/components/ai_service`
- chat 继续走现有 provider。
- STT/TTS 不再强行绑定 chat provider。
- 新增统一语音服务入口，内部调用 voice client。

3) `main/components/audio`
- 保留当前 I2S 与播放队列。
- 新增“录音分片读取接口”，用于流式 STT 推流。
- 新增“流式播放写入接口”，用于 TTS 边收边播。

4) `main/main.c`
- 新增语音状态机调度。
- 替换当前 `stt_callback` 入口为真实语音会话流程。
- 支持“播报期间打断并回到聆听”。

### 7.2 建议新增模块

建议新增 `main/components/voice_chat/`：
- `voice_session.h/.c`
  - 语音状态机
  - 会话生命周期管理
- `voice_gateway_client.h/.c`
  - WebSocket 管理
  - STT/TTS 抽象请求
- `voice_vad.h/.c`
  - 本地静音检测（可选，第一期可简化）

### 7.3 关键状态机

状态建议：
- `IDLE`
- `LISTENING`
- `RECOGNIZING`
- `THINKING`
- `SPEAKING`
- `INTERRUPTING`
- `ERROR`

状态迁移关键规则：
- LISTENING -> RECOGNIZING：检测到语音结束或用户松键。
- RECOGNIZING -> THINKING：收到 STT final。
- THINKING -> SPEAKING：收到首段 TTS 音频。
- SPEAKING -> INTERRUPTING：检测用户新一轮讲话或按键打断。
- 任意状态 -> ERROR：网络超时或协议异常。

## 8. 配置设计（建议）

新增配置字段（建议）

- provider 维度
  - `chat_provider`
  - `stt_provider`
  - `tts_provider`
- endpoint 维度
  - `chat_base_url`
  - `stt_base_url`
  - `tts_base_url`
  - `voice_gateway_url`
- model/voice 维度
  - `chat_model`
  - `stt_model`
  - `tts_voice`
- 运行参数
  - `enable_barge_in`
  - `vad_silence_ms`
  - `stt_timeout_ms`
  - `tts_timeout_ms`
  - `audio_chunk_ms`（默认 20）

兼容策略：
- 保留现有 `provider/base_url/model_name`，作为 chat 兼容字段。
- 新字段存在时优先使用新字段。

## 9. 错误处理与容灾

- 网络异常：指数退避重连（1s/2s/4s，最多 3 次）
- STT 无结果：超时返回并提示重试
- TTS 失败：回落为文本显示，不阻断会话
- 厂商限流：网关切换到备用厂商
- 设备内存紧张：降级分片缓冲区大小，优先保证链路不断

## 10. 安全与合规

- API Key 不放固件明文，统一放网关。
- 设备仅持有网关短期 token（可轮转）。
- 传输使用 TLS，校验证书链。
- 日志中脱敏用户隐私信息。

## 11. 性能目标（阶段目标）

- STT 首个中间结果 < 800ms
- STT 最终结果 < 2.0s（短句）
- TTS 首包可播 < 1.0s
- 单轮首次出声 < 3.0s
- 失败率（网络正常场景）< 3%

## 12. 开发计划（继续开发）

### 12.1 Phase A（当前迭代，先跑通）

- 新增 docs 技术方案（本文件）
- 定义 voice session 接口与状态机
- 实现按键说话 PTT 模式
- 接入网关版 STT final + chat + 整句 TTS 播放

交付结果：
- 可完成“说一句 -> 回一句”的稳定链路

### 12.2 Phase B（流式化）

- STT 中间结果上屏
- TTS 流式边收边播
- 播报可打断（barge-in）

交付结果：
- 交互延迟明显下降，接近实时体验

### 12.3 Phase C（产品化增强）

- 本地 VAD 与自动端点检测
- 多音色与语速配置
- 监控指标与错误统计上报

交付结果：
- 具备长期运行与运维能力

## 13. 验收标准

功能验收：
- 连续 20 轮中英文短句对话可用。
- 播报期间可被用户打断并进入下一轮。
- 断网恢复后可自动重连并继续使用。

稳定性验收：
- 连续运行 2 小时无崩溃。
- 内存无持续性泄漏（heap 波动可回收）。

体验验收：
- UI 状态与真实链路一致。
- 语音音量和清晰度在默认参数下可接受。

## 14. 与当前代码的落地映射

第一批优先修改文件：
- `main/components/config/config.h`
- `main/components/config/config.c`
- `main/Kconfig.projbuild`
- `main/main.c`
- `main/components/audio/audio.h`
- `main/components/audio/audio.c`
- `main/components/ai_service/ai_service.h`
- `main/components/ai_service/ai_service.c`

建议新增文件：
- `main/components/voice_chat/voice_session.h`
- `main/components/voice_chat/voice_session.c`
- `main/components/voice_chat/voice_gateway_client.h`
- `main/components/voice_chat/voice_gateway_client.c`

---

维护说明：
- 本文档是语音能力开发基线。
- 若后续厂商或协议调整，请先更新本文件再改代码，保证设计与实现一致。
