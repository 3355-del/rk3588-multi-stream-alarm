"""Build with GCC and run regressions using only the Python standard library.

Run from the project directory: python tools/check.py
Use --skip-build when the executables have already been compiled.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib

ROOT = Path(__file__).resolve().parents[1]
SUFFIX = ".exe" if os.name == "nt" else ""
BUILD = ROOT / "build"
HARDWARE_SOURCES = {"RealVideoSource.cpp","RealFrameProcessor.cpp","RealInferenceEngine.cpp","RkMppSource.cpp","DmaBuffer.cpp"}
CORE = sorted(str(p.relative_to(ROOT)) for p in (ROOT / "src").rglob("*.cpp") if p.name != "main.cpp" and p.name not in HARDWARE_SOURCES)

def build():
    compiler = shutil.which("g++")
    if not compiler:
        raise RuntimeError("g++ was not found; install a C++17 compiler or use CMake")
    BUILD.mkdir(exist_ok=True)
    common = [compiler, "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Wshadow",
              "-Wconversion", "-Wno-unused-parameter", "-pthread", "-Iinclude"]
    libs = ["-lws2_32"] if os.name == "nt" else []
    targets = [
        ("multi_stream_alarm_demo", CORE + ["src/main.cpp"], []),
        ("delivery_driver", CORE + ["tests/delivery_driver.cpp"], []),
        ("core_tests", CORE + ["tests/core_tests.cpp"], []),
        ("tensor_tests", ["tests/tensor_tests.cpp"], []),
        ("reconnect_tests", ["tests/reconnect_tests.cpp", "src/common/Logger.cpp"], []),
        ("alarm_tests", CORE + ["tests/alarm_tests.cpp"], []),
        ("module_tests", ["src/common/Logger.cpp", "src/hal/SimScene.cpp",
         "module/decode_infer/src/MockDecoder.cpp", "module/decode_infer/src/MockAiInfer.cpp",
         "module/decode_infer/src/MultiStreamEngine.cpp", "tests/module_tests.cpp"],
         ["-Imodule/decode_infer/include"]),
        ("decode_infer_demo", ["src/common/Logger.cpp", "src/hal/SimScene.cpp",
         "module/decode_infer/src/MockDecoder.cpp", "module/decode_infer/src/MockAiInfer.cpp",
         "module/decode_infer/src/MultiStreamEngine.cpp", "module/decode_infer/src/main.cpp"],
         ["-Imodule/decode_infer/include"]),
    ]
    if sys.platform.startswith("linux"):
        targets += [
            ("voice_tests", ["module/voice_mq/src/VoiceMqSim.cpp", "tests/voice_tests.cpp"],
             ["-Imodule/voice_mq/include", "-lrt"]),
            ("voice_mq_demo", ["module/voice_mq/src/VoiceMqSim.cpp", "module/voice_mq/src/main.cpp"],
             ["-Imodule/voice_mq/include", "-lrt"]),
        ]
    for name, sources, extra in targets:
        print(f"Building {name}", flush=True)
        # Relative paths also avoid MinGW ld failures with a non-ASCII project path.
        subprocess.run(common + sources + extra + libs + ["-o", str(BUILD/f"{name}{SUFFIX}")], cwd=ROOT, check=True)

def verify_png(path):
    data = path.read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", path
    position = 8
    compressed = bytearray()
    width = height = None
    ended = False
    while position < len(data):
        length = int.from_bytes(data[position:position+4], "big")
        tag = data[position+4:position+8]
        payload = data[position+8:position+8+length]
        crc = int.from_bytes(data[position+8+length:position+12+length], "big")
        assert len(payload) == length and zlib.crc32(tag+payload) == crc, (path, tag, "CRC")
        if tag == b"IHDR":
            width, height, depth, color, comp, filt, interlace = struct.unpack(">IIBBBBB", payload)
            assert (depth, color, comp, filt, interlace) == (8, 2, 0, 0, 0)
        elif tag == b"IDAT":
            compressed.extend(payload)
        elif tag == b"IEND":
            ended = True
        position += length + 12
    decoder = zlib.decompressobj()
    pixels = decoder.decompress(compressed) + decoder.flush()
    assert decoder.eof and not decoder.unused_data and position == len(data) and ended
    assert len(pixels) == (width*3+1)*height
    assert all(pixels[row*(width*3+1)] == 0 for row in range(height))
    return width, height

def exact(client, count):
    data = bytearray()
    while len(data) < count:
        piece = client.recv(count-len(data))
        if not piece:
            raise EOFError("truncated alarm")
        data.extend(piece)
    return bytes(data)

def alarm(client):
    client.settimeout(4)
    size = int.from_bytes(exact(client, 4), "big")
    assert 0 < size < 100000
    return json.loads(exact(client, size))

def run_core(directory):
    with (directory/"core.log").open("wb") as log:
        subprocess.run([str(BUILD/f"tensor_tests{SUFFIX}")], cwd=directory, stdout=log, stderr=subprocess.STDOUT, timeout=10, check=True)
        subprocess.run([str(BUILD/f"reconnect_tests{SUFFIX}")], cwd=directory, stdout=log, stderr=subprocess.STDOUT, timeout=10, check=True)
        subprocess.run([str(BUILD/f"core_tests{SUFFIX}")], cwd=directory, stdout=log, stderr=subprocess.STDOUT, timeout=15, check=True)
        subprocess.run([str(BUILD/f"alarm_tests{SUFFIX}")], cwd=directory, stdout=log, stderr=subprocess.STDOUT, timeout=10, check=True)
        subprocess.run([str(BUILD/f"module_tests{SUFFIX}")], cwd=directory, stdout=log, stderr=subprocess.STDOUT, timeout=10, check=True)
    assert verify_png(directory/"image_1280.png") == (1280,720)
    assert verify_png(directory/"image_128.png") == (128,96)
    assert verify_png(directory/"image_2.png") == (2,2)
    verify_png(directory/"outside.png")
    assert not (directory/"invalid.png").exists()
    print("PASS: IO failure shutdown, alarm rejection/retry/cancellation, unique names, shared snapshots, IO memory budget", flush=True)
    print("PASS: config, JSON, frame snapshot, move, NMS, polygon, queue, restart, PNG decoding, 100-target module", flush=True)

def run_network(directory):
    with socket.socket() as server:
        server.bind(("127.0.0.1", 0))
        port = server.getsockname()[1]
        cfg = {"frame_width":128, "frame_height":96, "fps":30, "queue_capacity":2, "run_seconds":7,
               "screenshot_dir":"network_shots", "alarm":{"host":"127.0.0.1","port":port},
               "streams":[{"id":17,"regions":[{"id":9,"cooldown_ms":500,
               "polygon":[[0,0],[128,0],[128,96],[0,96]]}]}]}
        (directory/"network.json").write_text(json.dumps(cfg), encoding="utf-8")
        with (directory/"network.log").open("wb") as log:
            process = subprocess.Popen([str(BUILD/f"multi_stream_alarm_demo{SUFFIX}"),"network.json"],
                                       cwd=directory, stdout=log, stderr=subprocess.STDOUT)
            try:
                # The first connect fails; a later alarm must reconnect when listening starts.
                time.sleep(1.3)
                assert process.poll() is None
                server.listen()
                server.settimeout(4)
                client, _ = server.accept()
                with client:
                    first = alarm(client)
                    assert first["stream_id"] == 17 and first["region_id"] == 9
                    assert first["screenshot"].startswith("alarm_s17_r9_")
                    verify_png(directory/"network_shots"/first["screenshot"])
                    # Force a reset, then require another connection and a valid framed message.
                    linger = struct.pack("HH" if os.name == "nt" else "ii", 1, 0)
                    client.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, linger)
                client, _ = server.accept()
                with client:
                    second = alarm(client)
                    assert second["stream_id"] == 17 and second["timestamp_ms"] >= first["timestamp_ms"]
                assert process.wait(timeout=10) == 0
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
        assert "sent=" in (directory/"network.log").read_text(errors="replace")
    print("PASS: delayed receiver, disconnect/reconnect, TCP framing, configured stream id, screenshot, timed exit", flush=True)

def run_unlimited(directory):
    cfg={"frame_width":128,"frame_height":96,"run_seconds":0,"screenshot_dir":"unlimited",
         "alarm":{"port":1},"streams":[{"id":10}]}
    (directory/"unlimited.json").write_text(json.dumps(cfg), encoding="utf-8")
    with (directory/"unlimited.log").open("wb") as log:
        process=subprocess.Popen([str(BUILD/f"multi_stream_alarm_demo{SUFFIX}"),"unlimited.json"],
                                 cwd=directory,stdout=log,stderr=subprocess.STDOUT)
        try:
            time.sleep(1.2)
            assert process.poll() is None, "run_seconds=0 exited"
            if os.name != "nt":
                process.send_signal(__import__("signal").SIGINT)
                assert process.wait(timeout=4)==0
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=4)
    print("PASS: run_seconds=0 stays alive (Windows test cleanup uses terminate; graceful stop tested in core)", flush=True)

def main():
    global BUILD
    parser=argparse.ArgumentParser()
    parser.add_argument("--skip-build",action="store_true")
    parser.add_argument("--build-dir",type=Path,default=BUILD)
    args=parser.parse_args()
    BUILD=args.build_dir.resolve()
    if not args.skip_build:
        build()
    # Keep reports and PNG evidence in the project build folder.
    directory=Path(tempfile.mkdtemp(prefix="regression-", dir=BUILD))
    print(f"Evidence: {directory}", flush=True)
    run_core(directory)
    subprocess.run([sys.executable,'-B','-m','unittest','discover','-s',str(ROOT/'tests'),'-p','test_detection_score.py'],check=True,timeout=10)
    run_network(directory)
    run_unlimited(directory)
    if sys.platform.startswith("linux"):
        with (directory/"voice.log").open("wb") as log:
            subprocess.run([str(BUILD/"voice_tests")],cwd=directory,stdout=log,stderr=subprocess.STDOUT,timeout=10,check=True)
        print("PASS: POSIX voice queue concurrent instances and restart")
    else:
        print("SKIP: POSIX voice queue runtime requires Linux")
    subprocess.run([sys.executable,str(ROOT/"tools/check_delivery.py"),str(BUILD/f"delivery_driver{SUFFIX}")],check=True,timeout=30)
    subprocess.run([sys.executable,'-X','utf8',str(ROOT/"tools/check_end_to_end.py"),str(BUILD)],check=True,timeout=30)
    print("All available regression tests passed.",flush=True)
if __name__=="__main__":
    main()
