#!/usr/bin/env python3
"""Host-side ASR/TTS smoke test against DashScope (千问/百炼).

Docs:
  ASR: https://platform.qianwenai.com/docs/developer-guides/speech/asr-realtime
  TTS: https://platform.qianwenai.com/docs/developer-guides/speech/realtime-streaming

Usage:
  set DASHSCOPE_API_KEY=sk-xxxx
  python tools/voice/test_asr_tts.py
"""

from __future__ import annotations

import json
import os
import sys
import threading
import time
import uuid
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT_DIR = ROOT / "tools" / "voice" / "out"
WS_URL = "wss://dashscope.aliyuncs.com/api-ws/v1/inference/"
TTS_TEXT = "你好，我是电子宠物，很高兴见到你。"
ASR_MODEL = "fun-asr-realtime"
TTS_MODEL = "cosyvoice-v3-flash"
TTS_VOICE = "longanyang"


def _require_key() -> str:
    key = os.environ.get("DASHSCOPE_API_KEY", "").strip()
    if not key:
        local_env = Path(__file__).resolve().parent / ".env.local"
        if local_env.is_file():
            for line in local_env.read_text(encoding="utf-8").splitlines():
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                name, value = line.split("=", 1)
                if name.strip() == "DASHSCOPE_API_KEY":
                    key = value.strip().strip('"').strip("'")
                    break
    if not key:
        print(
            "ERROR: set DASHSCOPE_API_KEY or create tools/voice/.env.local",
            file=sys.stderr,
        )
        raise SystemExit(2)
    print(f"API key present: len={len(key)} prefix={key[:4]}...")
    return key


def _ensure_deps() -> None:
    missing = []
    try:
        import websocket  # noqa: F401
    except ImportError:
        missing.append("websocket-client")
    try:
        import dashscope  # noqa: F401
    except ImportError:
        missing.append("dashscope")
    if missing:
        print("Installing:", ", ".join(missing))
        import subprocess

        subprocess.check_call([sys.executable, "-m", "pip", "install", "-q", *missing])


def run_tts(api_key: str, out_mp3: Path) -> Path:
    import dashscope
    from dashscope.audio.tts_v2 import SpeechSynthesizer

    dashscope.api_key = api_key
    dashscope.base_websocket_api_url = WS_URL.rstrip("/")

    print(f"TTS: model={TTS_MODEL} voice={TTS_VOICE}")
    print(f"TTS text: {TTS_TEXT}")
    synthesizer = SpeechSynthesizer(model=TTS_MODEL, voice=TTS_VOICE)
    audio = synthesizer.call(TTS_TEXT)
    if not audio:
        raise RuntimeError("TTS returned empty audio")

    out_mp3.parent.mkdir(parents=True, exist_ok=True)
    out_mp3.write_bytes(audio)
    print(
        f"TTS OK: bytes={len(audio)} file={out_mp3} "
        f"requestId={synthesizer.get_last_request_id()} "
        f"first_packet_ms={synthesizer.get_first_package_delay()}"
    )
    return out_mp3


def _pcm16_mono_to_wav(pcm: bytes, sample_rate: int, out_wav: Path) -> Path:
    out_wav.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(out_wav), "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        wf.writeframes(pcm)
    return out_wav


def run_tts_pcm_fallback(api_key: str, out_wav: Path) -> Path:
    """WebSocket TTS requesting PCM so ASR can consume wav without ffmpeg."""
    import websocket

    task_id = uuid.uuid4().hex[:32]
    done = threading.Event()
    started = threading.Event()
    errors: list[str] = []
    pcm_chunks: list[bytes] = []
    sample_rate = 16000

    def send_run_task(ws: websocket.WebSocketApp) -> None:
        msg = {
            "header": {"action": "run-task", "task_id": task_id, "streaming": "duplex"},
            "payload": {
                "task_group": "audio",
                "task": "tts",
                "function": "SpeechSynthesizer",
                "model": TTS_MODEL,
                "parameters": {
                    "text_type": "PlainText",
                    "voice": TTS_VOICE,
                    "format": "pcm",
                    "sample_rate": sample_rate,
                    "volume": 50,
                    "rate": 1,
                    "pitch": 1,
                    "enable_ssml": False,
                },
                "input": {},
            },
        }
        ws.send(json.dumps(msg))

    def send_text_and_finish(ws: websocket.WebSocketApp) -> None:
        cont = {
            "header": {"action": "continue-task", "task_id": task_id, "streaming": "duplex"},
            "payload": {"input": {"text": TTS_TEXT}},
        }
        ws.send(json.dumps(cont))
        fin = {
            "header": {"action": "finish-task", "task_id": task_id, "streaming": "duplex"},
            "payload": {"input": {}},
        }
        ws.send(json.dumps(fin))

    def on_open(ws: websocket.WebSocketApp) -> None:
        send_run_task(ws)

    def on_message(ws: websocket.WebSocketApp, message) -> None:
        if isinstance(message, (bytes, bytearray)):
            pcm_chunks.append(bytes(message))
            return
        data = json.loads(message)
        event = data.get("header", {}).get("event")
        if event == "task-started":
            started.set()
            send_text_and_finish(ws)
        elif event == "task-finished":
            done.set()
            ws.close()
        elif event == "task-failed":
            errors.append(data.get("header", {}).get("error_message", "task-failed"))
            done.set()
            ws.close()

    def on_error(ws: websocket.WebSocketApp, error) -> None:
        errors.append(str(error))
        done.set()

    print(f"TTS(WS/PCM): model={TTS_MODEL} voice={TTS_VOICE}")
    ws = websocket.WebSocketApp(
        WS_URL,
        header=[f"Authorization: bearer {api_key}"],
        on_open=on_open,
        on_message=on_message,
        on_error=on_error,
    )
    thread = threading.Thread(target=ws.run_forever, daemon=True)
    thread.start()
    if not done.wait(timeout=60):
        ws.close()
        raise TimeoutError("TTS websocket timed out")
    if errors:
        raise RuntimeError(f"TTS failed: {errors[0]}")
    pcm = b"".join(pcm_chunks)
    if not pcm:
        raise RuntimeError("TTS returned no PCM frames")
    path = _pcm16_mono_to_wav(pcm, sample_rate, out_wav)
    print(f"TTS OK: pcm_bytes={len(pcm)} wav={path}")
    return path


def run_asr(api_key: str, audio_file: Path, audio_format: str) -> str:
    import websocket

    task_id = uuid.uuid4().hex[:32]
    done = threading.Event()
    texts: list[str] = []
    errors: list[str] = []

    def send_run_task(ws: websocket.WebSocketApp) -> None:
        msg = {
            "header": {"action": "run-task", "task_id": task_id, "streaming": "duplex"},
            "payload": {
                "task_group": "audio",
                "task": "asr",
                "function": "recognition",
                "model": ASR_MODEL,
                "parameters": {"sample_rate": 16000, "format": audio_format},
                "input": {},
            },
        }
        ws.send(json.dumps(msg))

    def send_finish_task(ws: websocket.WebSocketApp) -> None:
        msg = {
            "header": {"action": "finish-task", "task_id": task_id, "streaming": "duplex"},
            "payload": {"input": {}},
        }
        ws.send(json.dumps(msg))

    def send_audio_stream(ws: websocket.WebSocketApp) -> None:
        chunk_size = 3200
        try:
            with audio_file.open("rb") as f:
                while True:
                    chunk = f.read(chunk_size)
                    if not chunk:
                        break
                    ws.send(chunk, opcode=websocket.ABNF.OPCODE_BINARY)
                    time.sleep(0.05)
            send_finish_task(ws)
        except Exception as exc:  # noqa: BLE001
            errors.append(str(exc))
            done.set()
            ws.close()

    def on_open(ws: websocket.WebSocketApp) -> None:
        send_run_task(ws)

    def on_message(ws: websocket.WebSocketApp, data: str) -> None:
        message = json.loads(data)
        event = message.get("header", {}).get("event")
        if event == "task-started":
            threading.Thread(target=send_audio_stream, args=(ws,), daemon=True).start()
        elif event == "result-generated":
            sentence = (
                message.get("payload", {})
                .get("output", {})
                .get("sentence", {})
                .get("text", "")
            )
            if sentence:
                texts.append(sentence)
                print(f"ASR partial/final: {sentence}")
        elif event == "task-finished":
            done.set()
            ws.close()
        elif event == "task-failed":
            errors.append(message.get("header", {}).get("error_message", "task-failed"))
            done.set()
            ws.close()

    def on_error(ws: websocket.WebSocketApp, error) -> None:
        errors.append(str(error))
        done.set()

    print(f"ASR: model={ASR_MODEL} file={audio_file} format={audio_format}")
    ws = websocket.WebSocketApp(
        WS_URL,
        header=[f"Authorization: bearer {api_key}"],
        on_open=on_open,
        on_message=on_message,
        on_error=on_error,
    )
    thread = threading.Thread(target=ws.run_forever, daemon=True)
    thread.start()
    if not done.wait(timeout=90):
        ws.close()
        raise TimeoutError("ASR websocket timed out")
    if errors:
        raise RuntimeError(f"ASR failed: {errors[0]}")
    if not texts:
        raise RuntimeError("ASR returned no text")
    return texts[-1]


def main() -> int:
    api_key = _require_key()
    _ensure_deps()
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    wav_path = OUT_DIR / "tts_probe.wav"
    try:
        # Prefer PCM/WAV path so ASR does not need ffmpeg for mp3 decode.
        run_tts_pcm_fallback(api_key, wav_path)
        asr_format = "wav"
        asr_input = wav_path
    except Exception as exc:  # noqa: BLE001
        print(f"TTS PCM path failed ({exc}); falling back to SDK mp3")
        mp3_path = OUT_DIR / "tts_probe.mp3"
        run_tts(api_key, mp3_path)
        asr_format = "mp3"
        asr_input = mp3_path

    text = run_asr(api_key, asr_input, asr_format)
    print("==== RESULT ====")
    print(f"ASR final text: {text}")
    print(f"artifacts: {asr_input}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:  # noqa: BLE001
        print(f"FAILED: {type(exc).__name__}: {exc}", file=sys.stderr)
        raise SystemExit(1)
