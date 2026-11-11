#!/usr/bin/env python3
"""Run the archive's host-only regressions. This never connects to a device."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run(command):
    result = subprocess.run(command, capture_output=True, text=True, timeout=90)
    if result.returncode:
        raise RuntimeError(" ".join(map(str, command)) + "\n" +
                           (result.stdout + result.stderr)[-7000:])
    return result.stdout.strip()


def main():
    if os.name != "posix":
        raise SystemExit("Run this host-only suite under Linux or WSL.")
    cc = os.environ.get("CC", "cc")
    cxx = os.environ.get("CXX", "c++")
    flags = ["-g", "-O1", "-Wall", "-Wextra", "-Werror", "-pthread",
             "-fsanitize=address,undefined", "-DVM_RAM_MIB=5120"]
    count = 0
    with tempfile.TemporaryDirectory(prefix="xnu-iqoo13-host-") as directory:
        work = Path(directory)
        for source in sorted((ROOT / "src/host").glob("test-*.c")):
            executable = work / source.stem
            run([cc, *flags, str(source), "-o", str(executable)])
            output = run([str(executable)])
            print(f"PASS {source.stem}: {output[-300:]}", flush=True)
            count += 1

        # Exercise the real guest worker with modeled IOKit, clock and MMIO.
        # Replace only external headers and its architecture-specific barrier.
        source = ROOT / "src/guest/GunyahTouch.cpp"
        barrier = '__asm__ volatile("dsb sy" ::: "memory");'
        body = source.read_text()
        if body.count(barrier) != 1:
            raise RuntimeError("Guest touch worker barrier changed; review the host fixture.")
        body = "\n".join(line for line in body.splitlines()
                         if not line.startswith("#include "))
        body = body.replace(barrier, "__atomic_thread_fence(__ATOMIC_SEQ_CST);")
        (work / "guest-worker-under-test.inc").write_text(body)
        executable = work / "guest-touch"
        run([cxx, "-std=c++17", *flags, "-I", str(work), "-I",
             str(source.parent), str(ROOT / "src/host/test-guest-touch.cpp"),
             "-o", str(executable)])
        output = run([str(executable)])
        if output != "GUEST_TOUCH_WORKER_PASS cases=14":
            raise RuntimeError("Unexpected guest-worker test result: " + output)
        print("PASS guest-touch: " + output, flush=True)
        count += 1
    print(f"PASS {count} host-only suites", flush=True)


if __name__ == "__main__":
    main()
