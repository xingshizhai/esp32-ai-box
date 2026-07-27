# ElatoAI AI 对话能力迁移说明（ESP-IDF / ESP32-S3-BOX-3）

## 目标
将 ElatoAI 的“多模型 AI 对话能力”迁移到当前 ESP-IDF 项目，并保持现有显示、语音链路、调试能力。

## 已完成（本仓库）
1. 聊天 Provider 扩展到 6 个：OpenAI / Zhipu / DeepSeek / Kimi / MiniMax / OpenRouter。
2. 通过 menuconfig 选择默认聊天 Provider，并分别配置每个 Provider 的 API Key / Base URL / Model。
3. Provider 配置持久化到 NVS，支持重启后保持。
4. Kimi、MiniMax、OpenRouter 使用 OpenAI-compatible 接口路径接入（复用现有 HTTP + JSON 处理链路）。
5. 保留并增强调试能力：
   - 启动前校验当前 Provider 配置是否完整。
   - OpenAI-compatible 请求失败时打印 HTTP 错误体并填充 response.error_msg。
6. OpenRouter 增加可选请求头配置（HTTP-Referer / X-Title），并在运行时自动注入。
7. 启动时输出聊天配置自检日志（provider/base_url/model，API key 脱敏），便于串口快速排查配置问题。
8. 支持串口运行时命令 `provider set <name>` 动态切换 provider，并在目标初始化失败时自动回滚。
9. 支持串口运行时命令更新当前 provider 的 key/base/model，并可通过 `provider test` 即时验证链路。
10. 支持 `provider preset <name>` 一键应用推荐 base/model，及 OpenRouter `referer/title` 运行时头配置。

## 与 ElatoAI 的能力映射
1. ElatoAI 的核心是“设备端 + 服务端网关 + 模型服务”的解耦。
2. 当前工程已具备相同关键分层：
   - UI 与显示：已实现（LVGL）
   - 聊天服务抽象：已实现（ai_service + providers）
   - 语音链路：已实现（voice gateway + STT/TTS）
3. 本次迁移聚焦“多模型对话”而非 Arduino 固件实现，满足 ESP-IDF 约束。

## menuconfig 建议
路径：Ai-Box Configuration -> Chat Provider Profiles

按 Provider 分别设置：
1. API Key
2. Base URL
3. Model

建议：
1. 密钥不要写入源码文件，使用 menuconfig 或设备端配置下发。
2. OpenRouter 常用地址：https://openrouter.ai/api/v1/chat/completions
3. Kimi 常用地址：https://api.moonshot.cn/v1/chat/completions
4. MiniMax 若使用非 OpenAI-compatible 端点，需要追加专用 provider 适配。
5. OpenRouter 可选配置 `OPENROUTER_HTTP_REFERER_DEFAULT` 与 `OPENROUTER_X_TITLE_DEFAULT`。

## 联调步骤
1. idf.py menuconfig
2. 选择默认 AI Provider
3. 填写该 Provider 的 key / base_url / model
4. idf.py build
5. idf.py -p <PORT> flash monitor
6. 在 UI/日志中确认 provider 名称与响应结果

## 已知限制
1. 当前 MiniMax 默认按 OpenAI-compatible 方式调用；若服务端字段不兼容，需要专用请求体。
2. 目前仅完成“对话文本推理”多 provider 扩展，STT/TTS provider 仍走现有 voice profile 逻辑。

## 下一步（可选）
1. 新增 minimax.c（专用请求格式）并接入 ai_service_create。
2. 新增 OpenRouter 可选头（HTTP-Referer / X-Title）配置项。
3. 增加运行时 provider 切换入口（UI 设置页或串口命令）。
