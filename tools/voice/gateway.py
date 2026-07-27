#!/usr/bin/env python3
"""Local Voice Gateway for ESP32-AI-Box.

Implements the HTTP contract expected by
`main/components/voice_chat/voice_gateway_client.c`:

  POST /v1/stt/start
  POST /v1/stt/chunk?session_id=...
  POST /v1/stt/stop
  POST /v1/tts
  GET  /health

Upstream: DashScope / 百炼 / 千问 (Fun-ASR + CosyVoice).

Usage:
  # tools/voice/.env.local must contain DASHSCOPE_API_KEY=sk-...
  python tools/voice/gateway.py
  python tools/voice/gateway.py --host 0.0.0.0 --port 8787
"""

from __future__ import annotations

import argparse
import logging
import os
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from dashscope_speech import (  # noqa: E402
    ASR_MODEL_DEFAULT,
    TTS_MODEL_DEFAULT,
    TTS_VOICE_DEFAULT,
    recognize_pcm,
    synthesize_pcm,
)

log = logging.getLogger("voice_gateway")

SESSION_TTL_S = 300
MAX_PCM_BYTES = 2 * 1024 * 1024  # ~60s @ 16kHz mono s16le

# ESP32 may still advertise Volcengine profile names (e.g. "bigmodel").
# This gateway always talks to DashScope, so remap unknown names to defaults.
_ASR_MODEL_PREFIXES = ("fun-asr", "paraformer", "gummy", "sensevoice")
_TTS_MODEL_PREFIXES = ("cosyvoice", "sambert", "qwen-tts")
_VOLCENGINE_ASR_ALIASES = frozenset({"bigmodel", "bigmodel_async", "bigmodel_nostream"})


def resolve_asr_model(requested: str | None) -> str:
    name = (requested or "").strip()
    if not name or name in _VOLCENGINE_ASR_ALIASES:
        return ASR_MODEL_DEFAULT
    lowered = name.lower()
    if any(lowered.startswith(p) for p in _ASR_MODEL_PREFIXES):
        return name
    log.warning("Ignoring non-DashScope ASR model %r, using %s", name, ASR_MODEL_DEFAULT)
    return ASR_MODEL_DEFAULT


def resolve_tts_model(requested: str | None) -> str:
    name = (requested or "").strip()
    if not name:
        return TTS_MODEL_DEFAULT
    lowered = name.lower()
    if any(lowered.startswith(p) for p in _TTS_MODEL_PREFIXES):
        return name
    log.warning("Ignoring non-DashScope TTS model %r, using %s", name, TTS_MODEL_DEFAULT)
    return TTS_MODEL_DEFAULT


def resolve_tts_voice(requested: str | None) -> str:
    name = (requested or "").strip()
    if not name:
        return TTS_VOICE_DEFAULT
    # CosyVoice voices are typically "long*" / "loong*"; Volcengine uses other IDs.
    lowered = name.lower()
    if lowered.startswith(("long", "loong", "zh_", "en_")):
        return name
    log.warning("Ignoring non-DashScope TTS voice %r, using %s", name, TTS_VOICE_DEFAULT)
    return TTS_VOICE_DEFAULT


@dataclass
class SttSession:
    session_id: str
    sample_rate: int = 16000
    model: str = ASR_MODEL_DEFAULT
    pcm: bytearray = field(default_factory=bytearray)
    created_at: float = field(default_factory=time.time)
    chunk_count: int = 0


class SessionStore:
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._sessions: dict[str, SttSession] = {}

    def create(self, session_id: str, sample_rate: int, model: str) -> SttSession:
        with self._lock:
            self._purge_locked()
            sess = SttSession(session_id=session_id, sample_rate=sample_rate, model=model)
            self._sessions[session_id] = sess
            return sess

    def get(self, session_id: str) -> SttSession | None:
        with self._lock:
            self._purge_locked()
            return self._sessions.get(session_id)

    def append(self, session_id: str, pcm: bytes) -> SttSession:
        with self._lock:
            self._purge_locked()
            sess = self._sessions.get(session_id)
            if sess is None:
                raise KeyError(session_id)
            if len(sess.pcm) + len(pcm) > MAX_PCM_BYTES:
                raise ValueError("pcm buffer too large")
            sess.pcm.extend(pcm)
            sess.chunk_count += 1
            return sess

    def pop(self, session_id: str) -> SttSession | None:
        with self._lock:
            return self._sessions.pop(session_id, None)

    def _purge_locked(self) -> None:
        now = time.time()
        dead = [sid for sid, s in self._sessions.items() if now - s.created_at > SESSION_TTL_S]
        for sid in dead:
            log.warning("expire idle STT session %s", sid)
            self._sessions.pop(sid, None)


def load_env_local() -> dict[str, str]:
    values: dict[str, str] = {}
    env_path = ROOT / ".env.local"
    if not env_path.is_file():
        return values
    for line in env_path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        name, value = line.split("=", 1)
        values[name.strip()] = value.strip().strip('"').strip("'")
    return values


def resolve_api_key() -> str:
    key = os.environ.get("DASHSCOPE_API_KEY", "").strip()
    if not key:
        key = load_env_local().get("DASHSCOPE_API_KEY", "").strip()
    if not key:
        raise SystemExit(
            "Missing DASHSCOPE_API_KEY. Set env or create tools/voice/.env.local"
        )
    return key


def ensure_deps() -> None:
    missing: list[str] = []
    try:
        import flask  # noqa: F401
    except ImportError:
        missing.append("flask")
    try:
        import websocket  # noqa: F401
    except ImportError:
        missing.append("websocket-client")
    if missing:
        import subprocess

        log.info("Installing: %s", ", ".join(missing))
        subprocess.check_call([sys.executable, "-m", "pip", "install", "-q", *missing])


def create_app(api_key: str, gateway_token: str = "") -> Any:
    from flask import Flask, Response, jsonify, request

    app = Flask("esp32_voice_gateway")
    store = SessionStore()
    app.config["API_KEY"] = api_key
    app.config["GATEWAY_TOKEN"] = gateway_token
    app.config["STORE"] = store

    def auth_ok() -> bool:
        expected = app.config["GATEWAY_TOKEN"]
        if not expected:
            return True
        auth = request.headers.get("Authorization", "")
        if auth.lower().startswith("bearer "):
            return auth[7:].strip() == expected
        return False

    @app.get("/health")
    def health():
        return jsonify({"ok": True, "service": "esp32-voice-gateway", "upstream": "dashscope"})

    @app.post("/v1/stt/start")
    def stt_start():
        if not auth_ok():
            return jsonify({"error": "unauthorized"}), 401
        body = request.get_json(silent=True) or {}
        session_id = str(body.get("session_id") or "").strip()
        if not session_id:
            return jsonify({"error": "session_id required"}), 400
        sample_rate = int(body.get("sample_rate") or 16000)
        requested_model = str(body.get("model_name") or "").strip()
        model = resolve_asr_model(requested_model)
        provider = str(body.get("provider") or "").strip()
        store.create(session_id, sample_rate=sample_rate, model=model)
        log.info(
            "STT start session=%s rate=%d provider=%s requested_model=%s resolved_model=%s",
            session_id,
            sample_rate,
            provider or "-",
            requested_model or "-",
            model,
        )
        return jsonify({"ok": True, "session_id": session_id})

    @app.post("/v1/stt/chunk")
    def stt_chunk():
        if not auth_ok():
            return jsonify({"error": "unauthorized"}), 401
        session_id = (request.args.get("session_id") or "").strip()
        if not session_id:
            return jsonify({"error": "session_id query required"}), 400
        pcm = request.get_data(cache=False) or b""
        if not pcm:
            return jsonify({"error": "empty pcm"}), 400
        try:
            sess = store.append(session_id, pcm)
        except KeyError:
            return jsonify({"error": "unknown session"}), 404
        except ValueError as exc:
            return jsonify({"error": str(exc)}), 413
        return jsonify(
            {
                "ok": True,
                "session_id": session_id,
                "chunk_count": sess.chunk_count,
                "pcm_bytes": len(sess.pcm),
            }
        )

    @app.post("/v1/stt/stop")
    def stt_stop():
        if not auth_ok():
            return jsonify({"error": "unauthorized"}), 401
        body = request.get_json(silent=True) or {}
        session_id = str(body.get("session_id") or "").strip()
        if not session_id:
            return jsonify({"error": "session_id required"}), 400
        sess = store.pop(session_id)
        if sess is None:
            return jsonify({"error": "unknown session"}), 404
        if not sess.pcm:
            log.warning("STT stop session=%s empty pcm", session_id)
            return jsonify({"text": "", "session_id": session_id})

        t0 = time.time()
        try:
            text = recognize_pcm(
                app.config["API_KEY"],
                bytes(sess.pcm),
                sample_rate=sess.sample_rate,
                model=sess.model,
                on_partial=lambda t: log.debug("ASR partial session=%s text=%s", session_id, t),
            )
        except Exception as exc:  # noqa: BLE001
            log.exception("STT upstream failed session=%s", session_id)
            return jsonify({"error": str(exc), "session_id": session_id}), 502

        elapsed_ms = int((time.time() - t0) * 1000)
        log.info(
            "STT stop session=%s chunks=%d pcm=%d bytes text_len=%d elapsed_ms=%d text=%s",
            session_id,
            sess.chunk_count,
            len(sess.pcm),
            len(text),
            elapsed_ms,
            text,
        )
        return jsonify({"text": text, "session_id": session_id, "elapsed_ms": elapsed_ms})

    @app.post("/v1/tts")
    def tts():
        if not auth_ok():
            return jsonify({"error": "unauthorized"}), 401
        body = request.get_json(silent=True) or {}
        text = str(body.get("text") or "").strip()
        session_id = str(body.get("session_id") or "").strip()
        if not text:
            return jsonify({"error": "text required"}), 400
        sample_rate = int(body.get("sample_rate") or 16000)
        model = resolve_tts_model(str(body.get("model_name") or ""))
        voice = resolve_tts_voice(str(body.get("voice") or ""))

        t0 = time.time()
        try:
            pcm = synthesize_pcm(
                app.config["API_KEY"],
                text,
                model=model,
                voice=voice,
                sample_rate=sample_rate,
            )
        except Exception as exc:  # noqa: BLE001
            log.exception("TTS upstream failed session=%s", session_id or "-")
            return jsonify({"error": str(exc), "session_id": session_id}), 502

        elapsed_ms = int((time.time() - t0) * 1000)
        log.info(
            "TTS ok session=%s text_len=%d pcm=%d bytes elapsed_ms=%d model=%s voice=%s",
            session_id or "-",
            len(text),
            len(pcm),
            elapsed_ms,
            model,
            voice,
        )
        return Response(
            pcm,
            status=200,
            mimetype="application/octet-stream",
            headers={
                "X-Audio-Format": "pcm_s16le",
                "X-Audio-Sample-Rate": str(sample_rate),
                "X-Audio-Channels": "1",
                "X-Elapsed-Ms": str(elapsed_ms),
            },
        )

    return app


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="0.0.0.0", help="bind address (default 0.0.0.0)")
    parser.add_argument("--port", type=int, default=8787, help="listen port (default 8787)")
    parser.add_argument(
        "--token",
        default=os.environ.get("VOICE_GATEWAY_TOKEN", ""),
        help="optional Bearer token expected from ESP32",
    )
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )

    ensure_deps()
    api_key = resolve_api_key()
    log.info("DashScope key loaded: len=%d prefix=%s...", len(api_key), api_key[:4])
    if args.token:
        log.info("Gateway auth token enabled")
    else:
        log.info("Gateway auth token disabled (open LAN access)")

    app = create_app(api_key, gateway_token=args.token.strip())
    log.info("Voice gateway listening on http://%s:%d", args.host, args.port)
    log.info("ESP32 menuconfig URL example: http://<this-pc-lan-ip>:%d", args.port)
    app.run(host=args.host, port=args.port, threaded=True, use_reloader=False)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
