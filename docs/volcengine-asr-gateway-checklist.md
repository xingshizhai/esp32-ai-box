# Volcengine ASR Gateway Minimal Validation Checklist

## 1. Goal

Validate that the full device -> gateway -> Volcengine ASR/TTS pipeline works with Volcengine credential triplet:

- APP ID
- Access Token
- Secret Key

And verify streaming behavior for different modes.

## 2. Prerequisites

- Device config is filled:
  - CONFIG_VOICE_VOLCENGINE_APP_ID
  - CONFIG_VOICE_VOLCENGINE_ACCESS_TOKEN
  - CONFIG_VOICE_VOLCENGINE_SECRET_KEY
- Voice gateway is enabled:
  - CONFIG_ENABLE_VOICE_GATEWAY=y
- Voice gateway URL is not empty:
  - CONFIG_DEFAULT_VOICE_GATEWAY_URL must be reachable from device network.
- Build and flash are successful.

## 3. Device -> Gateway Contract

Current device client calls these gateway APIs:

- POST /v1/stt/start
- POST /v1/stt/chunk?session_id=...
- POST /v1/stt/stop
- POST /v1/tts

Gateway should map these requests to Volcengine WebSocket upstream.

## 4. Required Start Payload Checks

On gateway, verify STT start payload contains:

- provider = volcengine
- model_name = selected model (recommended: bigmodel_async)
- provider_app_id and app_id
- provider_access_token and access_token
- provider_secret_key and secret_key
- format = pcm_s16le
- sample_rate = 16000
- channels = 1

Note:

- Device sends Volcengine triplet fields; provider_api_key may still mirror access_token for generic gateway compatibility.
- Gateway should prefer provider_app_id/provider_access_token/provider_secret_key.

## 5. Streaming Packet Checks

Recommended settings from Volcengine doc:

- Chunk duration: 100 to 200 ms
- Send interval: 100 to 200 ms
- For bidirectional mode, 200 ms is preferred

For 16 kHz, 16-bit mono PCM, 200 ms chunk is about 6400 bytes.

## 6. Test Matrix

Run at least 10 rounds per mode.

### Case A: bigmodel_async (recommended)

- Upstream endpoint should be async mode.
- Expected behavior: gateway receives result updates only when text changes.
- Metrics:
  - first partial latency
  - final latency
  - text accuracy

### Case B: bigmodel (bidirectional)

- Upstream endpoint should be bidirectional mode.
- Expected behavior: more frequent return packets.
- Compare with async for latency and stability.

### Case C: bigmodel_nostream (stream input mode)

- Upstream endpoint should be nostream mode.
- Expected behavior: final result after enough audio or final negative packet.
- Compare final accuracy vs latency.

### TTS upstream mode check

- Volcengine TTS HTTP (non-stream): https://openspeech.bytedance.com/api/v1/tts
- Volcengine TTS WebSocket (stream): wss://openspeech.bytedance.com/api/v1/tts/ws_binary
- If using WebSocket mode, ensure gateway sends `operation=submit` and keeps one synthesis per request flow.
- If using HTTP mode, ensure gateway decodes base64 audio payload before returning PCM bytes to device.

## 7. Pass Criteria

- Credential auth succeeds, no auth errors from upstream.
- STT final text is not empty in >= 9/10 rounds.
- TTS response audio size > 0 in >= 9/10 rounds.
- No obvious packet pacing warnings (too large or too small chunk/interval).

## 8. Suggested Gateway Log Fields

Log one line per session with:

- session_id
- mode and upstream URL
- chunk_ms and total_chunks
- audio_duration_ms
- first_partial_ms
- final_ms
- final_text_length
- tts_audio_bytes
- error_code and error_message (if any)

Do not print full access_token or secret_key in logs.

## 9. Fast Failure Triage

If device logs show "Voice gateway enabled but URL is empty":

- Fill CONFIG_DEFAULT_VOICE_GATEWAY_URL and rebuild.

If device logs show "Voice test unavailable: network is disconnected":

- Check Wi-Fi connectivity and DNS reachability.

If device logs show "Voice test unavailable: gateway or AI service is not ready":

- Check gateway URL, token, and startup path.

If gateway returns non-2xx on /v1/stt/start:

- Check triplet mapping and upstream auth headers/body.

If /v1/stt/stop returns empty text:

- Check chunk pacing, VAD/end packet handling, and mode mapping.
