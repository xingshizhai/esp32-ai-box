# ESP32 Dual AI Pets

[中文](README.md) | [English](README.en.md)

This repository contains two independently configurable and buildable ESP-IDF applications:

- `ai-pet/`: Xiaozhi, a friendly AI pet for short voice conversations with the user.
- `anti-pet/`: Shishen, an adversarial AI pet that challenges Xiaozhi to expose memory, logic, factual, and stability problems.

Roles are not tied to hardware. Either application can target ESP32-S3-BOX-3,
ESP32-S3-LCD-EV-BOARD-2, or a future supported board.

## Features

- On-device microphone capture and speaker playback.
- Alibaba Cloud DashScope ASR/TTS through an embedded ESP32 voice gateway, with an optional external gateway.
- DeepSeek, OpenAI, and Zhipu chat providers.
- Multi-turn conversation context.
- Chinese-capable LVGL interface.
- On-device WakeNet wake-word detection.
- Serial diagnostics, remote screenshots, and synthetic touch input.
- Closed-loop voice stress testing between two devices.

Default deployment:

| Project | Role | Default board | Default port | Start conversation |
|---|---|---|---|---|
| `ai-pet` | Xiaozhi | ESP32-S3-BOX-3 | `/dev/ttyACM0` | Say “你好，小智” or press the screen button |
| `anti-pet` | Shishen | ESP32-S3-LCD-EV-BOARD-2 | `/dev/ttyUSB0` | Press “挑战小智”; the first round wakes Xiaozhi automatically |

“你好，十神” requires a separately trained WakeNet model. Until that model is integrated,
Shishen starts conversations from its screen button.

## Repository Layout

```text
esp32-ai-box/
├── ai-pet/                 # Xiaozhi: independent ESP-IDF project
├── anti-pet/               # Shishen: independent ESP-IDF project
├── components/             # Shared application, UI, audio, and board components
├── assets/                 # Shared fonts and SPIFFS assets
├── cmake/                  # Shared build setup
├── docs/                   # Detailed design and test plans
└── tools/                  # Build, screenshot, input, and voice utilities
```

Each application has its own:

- `sdkconfig`: local Wi-Fi and API configuration; ignored by Git.
- `sdkconfig.defaults`: role defaults.
- `dependencies.lock`: committed dependency lock.
- `build/` and `managed_components/`: isolated local caches; ignored by Git.

Do not run `idf.py` from the repository root, and do not copy an entire `sdkconfig` between applications.

## Requirements

- ESP-IDF 6.0.1. The dependency set also supports a compatible ESP-IDF 5.2+ environment.
- An ESP32-S3 board and an accessible serial port.
- Access to the Espressif Component Registry for the first build.
- Wi-Fi, a chat-provider API key, and a DashScope API key when using the embedded voice gateway.

Load the local ESP-IDF environment, for example:

```bash
. /home/aladdin/.espressif/v6.0.1/esp-idf/export.sh
```

Use the actual `export.sh` path on your machine if it differs.

## Configure and Build ai-pet

### 1. Enter the project

```bash
cd ai-pet
```

### 2. Configure

```bash
idf.py menuconfig
```

Complete at least these settings:

1. `Ai-Box Configuration -> Target board`: select the connected board.
2. `Ai-Box Configuration -> WiFi SSID / WiFi Password`: enter the network settings.
3. `Ai-Box Configuration -> Default Chat Provider`: select a provider, then enter its API key
   under `Chat Provider Profiles`.
4. `Ai-Box Configuration -> Voice Configuration -> Voice Gateway`:
   - Prefer `Embedded on ESP32`;
   - enter the DashScope API key;
   - select `External HTTP gateway` only when a PC/server gateway is required.
5. `Voice Runtime`: ensure on-device WakeNet is enabled.

Save and exit. The result is stored in `ai-pet/sdkconfig`.

### 3. Build

```bash
idf.py build
```

### 4. Flash and monitor

```bash
idf.py -p /dev/ttyACM0 flash monitor
```

### 5. Verify

1. Confirm that the complete Xiaozhi screen is visible and Wi-Fi connects.
2. Confirm that the serial log contains `WakeNet ready`.
3. Say “你好，小智”, or press the conversation button.
4. Ask a short question.
5. Confirm ASR, chat response, and TTS playback, followed by a return to the idle state.

## Configure and Build anti-pet

### 1. Enter the project

```bash
cd anti-pet
```

### 2. Configure

```bash
idf.py menuconfig
```

Configure the board, Wi-Fi, chat API, and DashScope API as described for `ai-pet`.
The “你好，十神” WakeNet model is disabled by default because the custom model is not included.

### 3. Build

```bash
idf.py build
```

### 4. Flash and monitor

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

### 5. Verify

1. Confirm that the complete Shishen screen is visible and Wi-Fi connects.
2. Press “挑战小智”.
3. On the first round, confirm that Shishen says “你好，小智” and waits for Xiaozhi's reply.
4. Confirm that Shishen's ASR detects Xiaozhi saying “我在”. Only then may it generate and speak a challenge.
5. If “我在” is not detected, confirm that the UI asks for a retry and no challenge is spoken.
6. Confirm that the device does not reboot and returns to idle.

## Two-Device Closed-Loop Test

1. Flash `ai-pet` and `anti-pet` to separate devices.
2. Place the devices facing each other without putting a speaker directly against a microphone.
3. Monitor `/dev/ttyACM0` and `/dev/ttyUSB0` at the same time.
4. Press Shishen's “挑战小智” button. On the first round, Shishen says “你好，小智”,
   waits for “我在”, and starts its first challenge only after the handshake succeeds.
5. Wait for Xiaozhi to finish before starting the next challenge. The initial handshake is not
   repeated again during the same Shishen runtime.
6. Run at least five rounds and check context, false wake-ups, ASR/TTS failures, and reboots.

Do not allow unlimited automatic triggering. The test controller must enforce a round limit and a timeout per round.

See [PC and ESP32 audio loopback test](docs/pc_esp32_audio_loopback_test_plan.md) for the complete plan.

## Common Commands

Run these inside an application directory:

```bash
idf.py menuconfig       # Configure this application
idf.py build            # Build this application
idf.py fullclean        # Clear this application's caches
idf.py -p PORT flash    # Flash
idf.py -p PORT monitor  # Read serial logs
```

Or use the root helper:

```bash
tools/build_target.sh ai-pet build
tools/build_target.sh ai-pet flash /dev/ttyACM0
tools/build_target.sh anti-pet build
tools/build_target.sh anti-pet flash /dev/ttyUSB0
```

## Instructions for Agents

An automation agent should follow this order:

1. Select `ai-pet/` or `anti-pet/` for the task. Never build from the repository root.
2. Never print or commit Wi-Fi passwords or API keys from `sdkconfig`.
3. Never edit `managed_components/` directly.
4. Put shared behavior in `components/`; keep role behavior in the application's `main/app_role.c`.
5. Put board-specific code in `components/boards/<board>/`; never infer a role from a board.
6. Build both applications after changing a shared component.
7. Run `idf.py fullclean` after changing the board or dependency set.
8. Report the application, board, serial port, build result, and functional verification result.

Minimum delivery checks:

```bash
cd ai-pet && idf.py build
cd ../anti-pet && idf.py build
git diff --check
git status --short
```

## Debugging

- Serial: use `idf.py monitor` for reset, network, WakeNet, ASR, LLM, and TTS status.
- Screenshot after the device connects to Wi-Fi:

  ```bash
  python tools/screenshot/screenshot.py <DEVICE_IP> --out /tmp/device.png
  ```

- Synthetic touch:

  ```bash
  python tools/input/tap.py <DEVICE_IP> tap <X> <Y>
  ```

The default debug ports are `3333` for screenshots and `3334` for synthetic touch.

## Troubleshooting

### `idf.py fullclean` reports modified managed components

`managed_components/` is generated automatically. If a change is obsolete, back up only the
component named in the error and run `fullclean` again. If the change is required, move it into
a version-controlled project/private component. Do not bypass validation by editing
`.component_hash` or `CHECKSUMS.json`.

### A board change still uses old settings

Run inside the selected application:

```bash
idf.py fullclean
idf.py menuconfig
idf.py build
```

### Does a root sdkconfig take effect?

No. The active files are `ai-pet/sdkconfig` and `anti-pet/sdkconfig`. The root
`sdkconfig.defaults.esp32s3*` files are shared defaults and must remain.

### Why can “你好，十神” not be entered as a simple setting?

WakeNet uses an acoustic model, not text matching. A matching trained model must be obtained and
integrated. Shishen currently starts from its screen button. See
[wake-word model integration](docs/wake_word_change_plan.md).

## Additional Documentation

- [Wake-word model integration](docs/wake_word_change_plan.md)
- [PC and ESP32 audio loopback test](docs/pc_esp32_audio_loopback_test_plan.md)
- [Voice chat technical plan](docs/voice-chat-tts-stt-technical-plan.md)
