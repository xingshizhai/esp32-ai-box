#!/usr/bin/env python3
"""DashScope (百炼/千问) ASR/TTS helpers for the local voice gateway."""

from __future__ import annotations

import json
import threading
import time
import uuid
from typing import Callable

WS_URL = "wss://dashscope.aliyuncs.com/api-ws/v1/inference/"
ASR_MODEL_DEFAULT = "fun-asr-realtime"
TTS_MODEL_DEFAULT = "cosyvoice-v3-flash"
TTS_VOICE_DEFAULT = "longanyang"


def recognize_pcm(
    api_key: str,
    pcm: bytes,
    sample_rate: int = 16000,
    model: str = ASR_MODEL_DEFAULT,
    timeout_s: float = 90.0,
    on_partial: Callable[[str], None] | None = None,
) -> str:
    """Recognize mono PCM s16le via Fun-ASR realtime WebSocket."""
    import websocket

    if not pcm:
        return ""

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
                "model": model,
                "parameters": {"sample_rate": sample_rate, "format": "pcm"},
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
        chunk_size = 3200  # ~100ms @ 16kHz mono s16le
        try:
            offset = 0
            while offset < len(pcm):
                chunk = pcm[offset : offset + chunk_size]
                offset += len(chunk)
                ws.send(chunk, opcode=websocket.ABNF.OPCODE_BINARY)
                time.sleep(0.02)
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
                if on_partial is not None:
                    on_partial(sentence)
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

    ws = websocket.WebSocketApp(
        WS_URL,
        header=[f"Authorization: bearer {api_key}"],
        on_open=on_open,
        on_message=on_message,
        on_error=on_error,
    )
    thread = threading.Thread(target=ws.run_forever, daemon=True)
    thread.start()
    if not done.wait(timeout=timeout_s):
        ws.close()
        raise TimeoutError("ASR websocket timed out")
    if errors:
        raise RuntimeError(f"ASR failed: {errors[0]}")
    return texts[-1] if texts else ""


def synthesize_pcm(
    api_key: str,
    text: str,
    *,
    model: str = TTS_MODEL_DEFAULT,
    voice: str = TTS_VOICE_DEFAULT,
    sample_rate: int = 16000,
    timeout_s: float = 60.0,
) -> bytes:
    """Synthesize mono PCM s16le via CosyVoice WebSocket."""
    import websocket

    if not text.strip():
        return b""

    task_id = uuid.uuid4().hex[:32]
    done = threading.Event()
    errors: list[str] = []
    pcm_chunks: list[bytes] = []

    def send_run_task(ws: websocket.WebSocketApp) -> None:
        msg = {
            "header": {"action": "run-task", "task_id": task_id, "streaming": "duplex"},
            "payload": {
                "task_group": "audio",
                "task": "tts",
                "function": "SpeechSynthesizer",
                "model": model,
                "parameters": {
                    "text_type": "PlainText",
                    "voice": voice,
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
            "payload": {"input": {"text": text}},
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

    ws = websocket.WebSocketApp(
        WS_URL,
        header=[f"Authorization: bearer {api_key}"],
        on_open=on_open,
        on_message=on_message,
        on_error=on_error,
    )
    thread = threading.Thread(target=ws.run_forever, daemon=True)
    thread.start()
    if not done.wait(timeout=timeout_s):
        ws.close()
        raise TimeoutError("TTS websocket timed out")
    if errors:
        raise RuntimeError(f"TTS failed: {errors[0]}")
    pcm = b"".join(pcm_chunks)
    if not pcm:
        raise RuntimeError("TTS returned no PCM frames")
    return pcm
