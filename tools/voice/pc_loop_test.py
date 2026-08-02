#!/usr/bin/env python3
"""PC <-> ESP32 fully-voice closed-loop conversation driver.

Runs one or more *sessions*; each session is a fixed battery of turns that
escalate from simple to complex (memory set -> recall check -> reasoning ->
topic switch -> open-ended detail request), all spoken over the on-device
conversation history so later turns can be checked against earlier ones.

Two-step wake protocol per turn (matches how a human would actually use the
on-device WakeNet, and avoids racing the device's local "我在" ack against
the command -- the device has no AEC, so listening must not start until the
ack has finished playing; see components/app_core/app_runtime.c):
  1. PC speaks the wake phrase ALONE ("你好小智") and confirms via the
     device's serial log that WakeNet fired, retrying a few times before
     giving up (never speaks the query "blind" -- see wake_device()).
  2. PC speaks the actual query.
  3. PC starts recording *before* speaking the query and keeps recording
     until the device's serial log reports the round returned to IDLE/ERROR
     (or a generous timeout elapses) -- this chain is device ASR (cloud) +
     LLM (cloud) + TTS (cloud), so a fixed capture window is unreliable.
  4. The recording is transcribed with DashScope ASR (PC's independent
     view of what the device said), for cross-checking against the
     device's own serial-logged STT/response text.

While waiting for a turn to finish, screenshots are taken every few seconds
(not just at fixed before/after points) so a transient UI panel -- e.g. an
unexpected jump into the debug menu -- isn't missed between samples.

Also tails the device's own serial console (must already be captured to a
growing log file by a long-lived `cat /dev/ttyACM0 > LOG` process --
opening the port fresh resets the ESP32-S3 native USB-JTAG-serial, so this
script never opens the port itself) to pull the device-side STT text, AI
response, and round-completion marker for cross-checking.
"""
from __future__ import annotations

import argparse
import json
import math
import signal
import struct
import subprocess
import sys
import threading
import time
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parent
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from dashscope_speech import recognize_pcm, synthesize_pcm  # noqa: E402

import urllib.error
import urllib.request

WAKE_PHRASE = "你好小智"
CAPTURE_DEV = "hw:1,0"
CAPTURE_RATE = 44100
# PC is a single consistent female voice throughout -- wake phrase included
# -- so it never sounds like a different speaker mid-conversation. WakeNet
# is a keyword-spotting acoustic model and occasionally misses a given
# voice's prosody on the wake phrase specifically; measured against live
# hardware, "longwan_v3" (0/3) essentially never triggers it, while
# "longanhuan_v3" triggers ~75% per attempt. wake_device() already retries
# up to WAKE_MAX_ATTEMPTS times, so ~75%/attempt is >98% cumulative -- good
# enough to stay on one voice instead of switching to a male voice just for
# "你好小智". Re-check with tools/voice/dashscope_speech.py if this ever
# needs to change again; don't assume any CosyVoice voice triggers WakeNet
# without testing it against the real device first.
PC_VOICE_DEFAULT = "longanhuan_v3"  # CosyVoice female voice ("欢脱元气女")
WAKE_VOICE_DEFAULT = "longanhuan_v3"

PERSONA_NAME = "十神"
PERSONA_SYSTEM_PROMPT = (
    f"你叫{PERSONA_NAME}，是一个正在用语音测试智能音箱的真实用户，性格好奇、随和、有点爱开玩笑。"
    "你会收到设备刚才说的话（可能因为语音识别不完美有点走样）。"
    "请用一句简短、自然、口语化的中文（不超过20个字）继续这场对话，"
    "可以追问细节、可以自然地换话题、也可以表达情绪或吐槽，"
    "但不要加引号或旁白，只输出你要说的这句话。"
    "整场对话应该由简单到复杂：先问一些简单的记忆/复述类问题，"
    "随着对话推进逐渐问一些需要设备思考、计算或详细解释的问题。"
)

# Local wake ack ("我在") is ~1.3s of embedded PCM played fire-and-forget
# right after WakeNet fires; this is *not* a cloud round trip. Trimmed to
# the minimum that's still safe: firmware blocks LISTENING until ~1.3s ack
# + 150ms margin have elapsed (see app_runtime.c), so anything much below
# ~2.0s here risks the query's start getting clipped by capture not being
# open yet -- don't cut this further without retesting on hardware.
WAKE_ACK_WAIT_S = 2.0
# How many times to re-speak the wake phrase before giving up on a turn.
WAKE_MAX_ATTEMPTS = 3
# Device round = cloud ASR + cloud LLM + cloud TTS + on-device playback.
ROUND_TIMEOUT_S = 90.0
# Extra tail recorded after the device reports IDLE, to catch playback
# amplifier/speaker decay and avoid clipping the last word.
POST_ROUND_TAIL_S = 1.2
# How often to snapshot the screen while waiting for a round to finish.
SCREENSHOT_POLL_S = 4.0

# Escalating-difficulty turn battery for one session (mirrors
# docs/pc_esp32_audio_loopback_test_plan.md's round table, adapted for the
# two-step voice-only wake protocol instead of a manual "开始对话" tap).
DEFAULT_TURNS = [
    "请记住数字四十二",
    "我刚才让你记住的数字是多少",
    "把这个数字乘以二，简短回答就好",
    "请用一句话介绍一下ESP32",
    "刚才介绍ESP32的内容，可以再详细补充一点吗",
]


def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


PC_LABEL = f"PC({PERSONA_NAME})"
DEVICE_LABEL = "设备"
_LABEL_WIDTH = max(len(PC_LABEL), len(DEVICE_LABEL))


def chat_line(speaker: str, text: str, tag: str = "") -> None:
    """Print one turn of the live conversation transcript, right-padded so
    PC/device lines line up -- this is the primary output for demos, kept
    separate from the timestamped diagnostic log() lines."""
    ts = time.strftime("%H:%M:%S")
    suffix = f"  [{tag}]" if tag else ""
    print(f"  {ts}  {speaker:<{_LABEL_WIDTH}} : {text}{suffix}", flush=True)


def chat_rule(title: str) -> None:
    print(f"\n{'=' * 60}\n{title}\n{'=' * 60}", flush=True)


def play_wav(path: Path) -> None:
    subprocess.run(["aplay", str(path)], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def speak(dashscope_key: str, text: str, out_wav: Path, voice: str, sample_rate: int = 16000) -> None:
    pcm = synthesize_pcm(dashscope_key, text, voice=voice, sample_rate=sample_rate)
    with wave.open(str(out_wav), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sample_rate)
        w.writeframes(pcm)
    log(f"PC TTS ok (voice={voice}): {len(pcm)} bytes -> {out_wav.name}")
    play_wav(out_wav)


def take_screenshot(device_ip: str, out_png: Path) -> bool:
    try:
        subprocess.run(
            [sys.executable, str(ROOT.parent / "screenshot" / "screenshot.py"), device_ip, "--out", str(out_png)],
            check=True,
            timeout=10,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        return True
    except Exception as exc:  # noqa: BLE001
        log(f"screenshot failed: {exc}")
        return False


class ScreenshotPoller:
    """Snapshots the device screen on a background thread at a fixed cadence
    so short-lived UI states aren't missed between the coarse before/after
    checkpoints."""

    def __init__(self, device_ip: str, out_dir: Path, prefix: str, interval_s: float = SCREENSHOT_POLL_S):
        self._device_ip = device_ip
        self._out_dir = out_dir
        self._prefix = prefix
        self._interval_s = interval_s
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._count = 0

    def _run(self) -> None:
        while not self._stop.wait(self._interval_s):
            self._count += 1
            take_screenshot(self._device_ip, self._out_dir / f"{self._prefix}_poll{self._count:02d}.png")

    def start(self) -> None:
        if not self._device_ip:
            return
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=5)


def start_recording(out_wav: Path) -> subprocess.Popen:
    """Start arecord in the background; caller stops it once the round completes."""
    return subprocess.Popen(
        [
            "arecord",
            "-D", CAPTURE_DEV,
            "-f", "S16_LE",
            "-r", str(CAPTURE_RATE),
            "-c", "1",
            str(out_wav),
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def stop_recording(proc: subprocess.Popen) -> None:
    if proc.poll() is not None:
        return
    proc.send_signal(signal.SIGINT)
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.terminate()
        proc.wait(timeout=5)


def wav_metrics(path: Path) -> dict:
    with wave.open(str(path)) as w:
        frames = w.readframes(w.getnframes())
        rate = w.getframerate()
    if not frames:
        return dict(rate=rate, rms=0.0, peak=0, active=0.0)
    samples = struct.unpack("<%dh" % (len(frames) // 2), frames)
    n = len(samples)
    rms = math.sqrt(sum(v * v for v in samples) / n)
    peak = max(abs(v) for v in samples)
    active = sum(1 for v in samples if abs(v) > 500) / n
    return dict(rate=rate, rms=rms, peak=peak, active=active)


def pc_transcribe(dashscope_key: str, wav_path: Path) -> str:
    with wave.open(str(wav_path)) as w:
        pcm = w.readframes(w.getnframes())
        rate = w.getframerate()
    try:
        return recognize_pcm(dashscope_key, pcm, sample_rate=rate)
    except Exception as exc:  # noqa: BLE001
        log(f"PC ASR failed: {exc}")
        return ""


def tail_new(log_path: Path, start_offset: int) -> tuple[str, int]:
    if not log_path.exists():
        return "", start_offset
    with log_path.open("rb") as f:
        f.seek(start_offset)
        data = f.read()
    return data.decode("utf-8", errors="replace"), start_offset + len(data)


def deepseek_reply(api_key: str, base_url: str, model: str, history: list[dict]) -> str:
    body = json.dumps(
        {
            "model": model,
            "messages": history,
            "max_tokens": 80,
            "temperature": 0.8,
        }
    ).encode("utf-8")
    req = urllib.request.Request(
        base_url,
        data=body,
        headers={
            "Authorization": f"Bearer {api_key}",
            "Content-Type": "application/json",
        },
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=30) as resp:
        data = json.loads(resp.read().decode("utf-8"))
    return data["choices"][0]["message"]["content"].strip()


def wait_for_wake_ack(log_path: Path, start_offset: int, timeout_s: float = WAKE_ACK_WAIT_S) -> tuple[bool, int]:
    """Poll serial for 'WakeNet detected' as confirmation the ack round started."""
    deadline = time.time() + timeout_s
    offset = start_offset
    detected = False
    collected = ""
    while time.time() < deadline:
        chunk, offset = tail_new(log_path, offset)
        if chunk:
            collected = (collected + chunk)[-2000:]  # see wait_round_complete for why not just `chunk`
        if "WakeNet detected" in collected:
            detected = True
            break
        time.sleep(0.2)
    # Whether or not we saw the log line, give the ~1.3s local ack clip
    # time to finish playing before speaking over it.
    remaining = deadline - time.time()
    if remaining > 0:
        time.sleep(remaining)
    return detected, offset


def announce_round(dashscope_key: str, voice: str, out_dir: Path, tag: str, turn_no: int, n_turns: int) -> None:
    """Speak 'this is round N of M' on the PC speaker before each turn, so a
    live audience hears the round boundary the same way the transcript
    shows it as a '--- 第 N/M 轮 ---' header."""
    text = f"这是第{turn_no}轮会话测试，共{n_turns}轮"
    announce_wav = out_dir / f"{tag}_announce.wav"
    try:
        speak(dashscope_key, text, announce_wav, voice=voice)
    except Exception as exc:  # noqa: BLE001
        log(f"Round announcement TTS failed ({exc}); continuing without audio announcement")
    chat_line("  ▶", text)


def wake_device(dashscope_key: str, wake_voice: str, serial_log: Path, out_dir: Path, tag: str) -> bool:
    """Speak the wake phrase and confirm via serial log that WakeNet fired,
    retrying a few times before giving up. Returns False if never confirmed --
    callers must NOT proceed to speak the query in that case."""
    for attempt in range(1, WAKE_MAX_ATTEMPTS + 1):
        offset = serial_log.stat().st_size if serial_log.exists() else 0
        wake_wav = out_dir / f"{tag}_wake_attempt{attempt}.wav"
        retry_tag = "唤醒" if attempt == 1 else f"唤醒重试 {attempt}/{WAKE_MAX_ATTEMPTS}"
        speak(dashscope_key, WAKE_PHRASE, wake_wav, voice=wake_voice)
        chat_line(PC_LABEL, WAKE_PHRASE, retry_tag)

        detected, _ = wait_for_wake_ack(serial_log, offset)
        if detected:
            chat_line(DEVICE_LABEL, "我在", "已唤醒")
            return True

        log(f"No WakeNet detection after attempt {attempt}; device likely still asleep.")
        time.sleep(0.6)

    chat_line(DEVICE_LABEL, "(始终未响应唤醒词)", "跳过本轮")
    log(f"Giving up waking the device after {WAKE_MAX_ATTEMPTS} attempts -- skipping this turn's query.")
    return False


# Serial markers worth surfacing live as one-word progress pings while a
# round is in flight, so the transcript stream isn't silent for the
# 20-90s a cloud ASR+LLM+TTS round trip can take. Order matters: first
# match per chunk wins, and each is only announced once per round.
_PROGRESS_MARKERS = [
    ("LISTENING", "设备开始录音"),
    ("RECOGNIZING", "录音结束，识别中"),
    ("ASR upstream event=task-finished", "语音识别完成"),
    ("THINKING", "思考中"),
    ("AI_SERVICE: Chat with history", "请求大模型中"),
    ("SPEAKING", "语音合成开始"),
    ("Audio playback completed", "播报结束"),
]


def wait_round_complete(log_path: Path, start_offset: int, timeout_s: float = ROUND_TIMEOUT_S) -> dict:
    """Poll the serial log tail until the device round returns to IDLE/ERROR,
    printing a short progress ping for each stage transition so the live
    transcript stays active during the cloud round trip instead of going
    quiet for tens of seconds."""
    deadline = time.time() + timeout_s
    offset = start_offset
    result = {
        "device_stt_text": "",
        "device_ai_response": "",
        "final_state": "TIMEOUT",
        "reboot": False,
        "illegal_transition": False,
    }
    seen_markers: set[str] = set()
    last_heartbeat = time.time()
    # Accumulate the full text seen so far and match against *that*, not
    # just the newest chunk: serial bytes arrive on arbitrary read
    # boundaries, so a marker string (e.g. "--(TTS_END)--> IDLE") can be
    # split across two tail_new() reads and never match either chunk alone
    # -- that bug made completed rounds wait out the full 90s timeout even
    # though the device had already finished and gone idle. Bounded to the
    # last 4000 chars so a runaway round can't grow this unboundedly.
    collected = ""
    while time.time() < deadline:
        chunk, offset = tail_new(log_path, offset)
        if chunk:
            collected = (collected + chunk)[-4000:]

            for marker, note in _PROGRESS_MARKERS:
                if marker in collected and marker not in seen_markers:
                    seen_markers.add(marker)
                    chat_line("  ·", note)
                    last_heartbeat = time.time()

            if "Voice STT text:" in collected:
                for line in collected.splitlines():
                    if "Voice STT text:" in line:
                        result["device_stt_text"] = line.split("Voice STT text:", 1)[1].strip()
            if "AI response:" in collected:
                for line in collected.splitlines():
                    if "AI response:" in line:
                        result["device_ai_response"] = line.split("AI response:", 1)[1].strip()
            if "Guru Meditation" in collected or "rst:0x" in collected:
                result["reboot"] = True
            if "illegal transition" in collected:
                result["illegal_transition"] = True
            if "--(TTS_END)--> IDLE" in collected or "voice round reset" in collected:
                result["final_state"] = "IDLE"
                break
            if "--> ERROR" in collected:
                result["final_state"] = "ERROR"
                break
        elif time.time() - last_heartbeat > 8.0:
            # No new serial activity for a while (still within a single
            # stage, e.g. waiting on the cloud LLM) -- ping so the live
            # viewer knows the round hasn't stalled.
            chat_line("  ·", "等待设备响应中...")
            last_heartbeat = time.time()
        time.sleep(0.3)
    return result


def run_turn(args, serial_log: Path, out_dir: Path, tag: str, query: str) -> dict:
    t0 = time.time()

    if args.device_ip:
        take_screenshot(args.device_ip, out_dir / f"{tag}_0_idle.png")

    woke = wake_device(args.dashscope_key, args.wake_voice, serial_log, out_dir, tag)

    if args.device_ip:
        take_screenshot(args.device_ip, out_dir / f"{tag}_1_after_ack.png")

    if not woke:
        log("Skipping query for this turn -- device was never confirmed awake.")
        return {
            "tag": tag,
            "pc_said_wake": WAKE_PHRASE,
            "pc_said_query": None,
            "wakenet_log_seen": False,
            "device_final_state": "WAKE_FAILED",
            "elapsed_ms": int((time.time() - t0) * 1000),
        }

    round_offset = serial_log.stat().st_size if serial_log.exists() else 0
    rec_wav = out_dir / f"{tag}_esp32_reply.wav"
    rec_proc = start_recording(rec_wav)
    time.sleep(0.3)  # let arecord actually open the device before we start talking

    query_wav = out_dir / f"{tag}_pc_query.wav"
    speak(args.dashscope_key, query, query_wav, voice=args.pc_voice)
    chat_line(PC_LABEL, query)

    poller = ScreenshotPoller(args.device_ip, out_dir, f"{tag}_2_wait")
    poller.start()
    log(f"Waiting up to {ROUND_TIMEOUT_S:.0f}s for device round to finish (cloud ASR+LLM+TTS)...")
    device = wait_round_complete(serial_log, round_offset)
    poller.stop()

    time.sleep(POST_ROUND_TAIL_S)
    stop_recording(rec_proc)

    if args.device_ip:
        take_screenshot(args.device_ip, out_dir / f"{tag}_3_after.png")

    elapsed_ms = int((time.time() - t0) * 1000)
    metrics = wav_metrics(rec_wav)
    pc_heard = pc_transcribe(args.dashscope_key, rec_wav)

    status_tag = f"{device['final_state']}, {elapsed_ms / 1000:.1f}s"
    reply_text = device["device_ai_response"] or pc_heard or "(未识别到回复)"
    chat_line(DEVICE_LABEL, reply_text, status_tag)
    if device["device_stt_text"] and device["device_stt_text"] != query:
        chat_line("  └ 设备实际听到", device["device_stt_text"])
    if device["reboot"]:
        chat_line("  !", "设备在本轮期间重启了", "警告")

    log(f"Device STT (device-side): {device['device_stt_text']!r}")
    log(f"Device AI response      : {device['device_ai_response']!r}")
    log(f"Device final state      : {device['final_state']} reboot={device['reboot']} illegal_transition={device['illegal_transition']}")
    log(f"PC mic metrics          : rms={metrics['rms']:.1f} peak={metrics['peak']} active={metrics['active']:.1%}")
    log(f"PC ASR of ESP32 reply   : {pc_heard!r}")

    return {
        "tag": tag,
        "pc_said_wake": WAKE_PHRASE,
        "pc_said_query": query,
        "wakenet_log_seen": True,
        "device_stt_text": device["device_stt_text"],
        "device_ai_response": device["device_ai_response"],
        "device_final_state": device["final_state"],
        "device_reboot": device["reboot"],
        "device_illegal_transition": device["illegal_transition"],
        "pc_asr_of_reply": pc_heard,
        "audio_rms": metrics["rms"],
        "audio_peak": metrics["peak"],
        "audio_active_ratio": metrics["active"],
        "elapsed_ms": elapsed_ms,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dashscope-key", required=True)
    parser.add_argument(
        "--serial-log",
        default="/tmp/esp32_serial.log",
        help="path to the growing serial capture file (default: /tmp/esp32_serial.log -- "
        "start it with: stty -F /dev/ttyACM0 115200 cs8 -cstopb -parenb -ixon -ixoff raw -echo -hupcl "
        "&& cat /dev/ttyACM0 > /tmp/esp32_serial.log &)",
    )
    parser.add_argument(
        "--out-dir",
        default=None,
        help="default: tools/voice/out/<timestamp>",
    )
    parser.add_argument("--sessions", type=int, default=1, help="how many full turn-batteries to run back to back")
    parser.add_argument("--turns-per-session", type=int, default=len(DEFAULT_TURNS), help="dynamic mode only")
    parser.add_argument("--pc-voice", default=PC_VOICE_DEFAULT, help="DashScope CosyVoice voice id for the PC side")
    parser.add_argument("--wake-voice", default=WAKE_VOICE_DEFAULT, help="voice id used only for the wake phrase")
    parser.add_argument("--device-ip", default="", help="ESP32 IP for screenshots (skip if empty)")
    parser.add_argument(
        "--turns",
        nargs="+",
        default=None,
        help="override the default 5-turn easy->hard battery with custom queries (fixed mode only)",
    )
    parser.add_argument(
        "--deepseek-key",
        default="",
        help=f"if set, turns are generated live by the '{PERSONA_NAME}' persona via DeepSeek "
        "instead of the fixed battery",
    )
    parser.add_argument("--deepseek-url", default="https://api.deepseek.com/v1/chat/completions")
    parser.add_argument("--deepseek-model", default="deepseek-chat")
    args = parser.parse_args()

    dynamic = bool(args.deepseek_key)
    turns = args.turns if args.turns else DEFAULT_TURNS

    out_dir = Path(args.out_dir) if args.out_dir else ROOT / "out" / time.strftime("%Y%m%d_%H%M%S")
    out_dir.mkdir(parents=True, exist_ok=True)
    serial_log = Path(args.serial_log)
    if not serial_log.exists():
        print(
            f"warning: {serial_log} doesn't exist yet -- wake confirmation and device-side "
            "STT/response cross-checking need a live serial capture. Start one first:\n"
            f"  stty -F /dev/ttyACM0 115200 cs8 -cstopb -parenb -ixon -ixoff raw -echo -hupcl\n"
            f"  cat /dev/ttyACM0 > {serial_log} &\n",
            file=sys.stderr,
        )

    all_sessions = []
    for session_no in range(1, args.sessions + 1):
        n_turns = args.turns_per_session if dynamic else len(turns)
        mode_label = f"动态/{PERSONA_NAME}人设" if dynamic else "固定题库"
        chat_rule(f"会话 {session_no}/{args.sessions}  ·  {n_turns} 轮  ·  {mode_label}")
        session_report = []
        history = [{"role": "system", "content": PERSONA_SYSTEM_PROMPT}] if dynamic else None
        next_query = turns[0] if dynamic else None

        for turn_no in range(1, n_turns + 1):
            query = next_query if dynamic else turns[turn_no - 1]
            print(f"\n--- 第 {turn_no}/{n_turns} 轮 ---", flush=True)
            tag = f"s{session_no}_t{turn_no}"
            announce_round(args.dashscope_key, args.pc_voice, out_dir, tag, turn_no, n_turns)
            result = run_turn(args, serial_log, out_dir, tag, query)
            result["session"] = session_no
            result["turn"] = turn_no
            session_report.append(result)

            if dynamic and turn_no < n_turns:
                heard = result.get("pc_asr_of_reply") or "（没听清）"
                history.append({"role": "user", "content": f"设备刚才说：{heard}"})
                try:
                    next_query = deepseek_reply(args.deepseek_key, args.deepseek_url, args.deepseek_model, history)
                except (urllib.error.URLError, KeyError, json.JSONDecodeError) as exc:
                    log(f"DeepSeek call failed ({exc}); reusing a generic follow-up")
                    next_query = "再多说一点吧"
                history.append({"role": "assistant", "content": next_query})

            time.sleep(0.8)
        all_sessions.append(session_report)

    report_path = out_dir / "sessions_report.json"
    report_path.write_text(json.dumps(all_sessions, ensure_ascii=False, indent=2), encoding="utf-8")
    log(f"Report written to {report_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
