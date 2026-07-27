#!/usr/bin/env python3
"""Smoke-test the local voice gateway HTTP contract (no ESP32 required)."""

from __future__ import annotations

import json
import sys
import time
import urllib.error
import urllib.request
import uuid
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parent
OUT = ROOT / "out" / "tts_probe.wav"
BASE = sys.argv[1] if len(sys.argv) > 1 else "http://127.0.0.1:8787"


def http_json(method: str, path: str, body: dict | None = None, timeout: float = 120.0) -> dict:
    data = None if body is None else json.dumps(body).encode("utf-8")
    req = urllib.request.Request(
        BASE + path,
        data=data,
        method=method,
        headers={"Content-Type": "application/json"} if data is not None else {},
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        raw = resp.read()
        if not raw:
            return {}
        return json.loads(raw.decode("utf-8"))


def http_bytes(method: str, path: str, body: bytes, content_type: str, timeout: float = 120.0) -> bytes:
    req = urllib.request.Request(
        BASE + path,
        data=body,
        method=method,
        headers={"Content-Type": content_type},
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read()


def load_pcm_from_wav(path: Path) -> tuple[bytes, int]:
    with wave.open(str(path), "rb") as wf:
        assert wf.getnchannels() == 1
        assert wf.getsampwidth() == 2
        rate = wf.getframerate()
        pcm = wf.readframes(wf.getnframes())
    return pcm, rate


def main() -> int:
    print("health:", http_json("GET", "/health"))

    session_id = f"host_{uuid.uuid4().hex[:12]}"
    if not OUT.is_file():
        print(f"missing {OUT}; run tools/voice/test_asr_tts.py first to generate a probe wav")
        return 2

    pcm, rate = load_pcm_from_wav(OUT)
    print(f"probe wav: {OUT} rate={rate} pcm={len(pcm)} bytes")

    print("stt/start:", http_json("POST", "/v1/stt/start", {
        "session_id": session_id,
        "sample_rate": rate,
        "format": "pcm_s16le",
        "channels": 1,
        "model_name": "fun-asr-realtime",
    }))

    chunk = 3200
    for i in range(0, len(pcm), chunk):
        part = pcm[i : i + chunk]
        http_bytes("POST", f"/v1/stt/chunk?session_id={session_id}", part, "application/octet-stream")
    t0 = time.time()
    stop = http_json("POST", "/v1/stt/stop", {"session_id": session_id}, timeout=120)
    print(f"stt/stop ({int((time.time()-t0)*1000)} ms):", stop)

    text = stop.get("text") or "你好，电子宠物。"
    t0 = time.time()
    audio = http_bytes(
        "POST",
        "/v1/tts",
        json.dumps({
            "session_id": session_id,
            "text": text,
            "format": "pcm_s16le",
            "sample_rate": 16000,
            "voice": "longanyang",
            "model_name": "cosyvoice-v3-flash",
        }).encode("utf-8"),
        "application/json",
        timeout=120,
    )
    print(f"tts ok ({int((time.time()-t0)*1000)} ms): pcm={len(audio)} bytes")
    out_pcm = ROOT / "out" / "gateway_tts.pcm"
    out_pcm.write_bytes(audio)
    print("saved:", out_pcm)
    if not stop.get("text"):
        print("FAILED: empty ASR text")
        return 1
    if len(audio) < 1000:
        print("FAILED: TTS audio too small")
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except urllib.error.URLError as exc:
        print(f"FAILED: cannot reach gateway at {BASE}: {exc}", file=sys.stderr)
        raise SystemExit(1)
