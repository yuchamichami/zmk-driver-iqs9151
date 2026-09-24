#!/usr/bin/env python3
"""Run a qemu_cortex_m3 ZTEST ELF, require its explicit success verdict."""
import argparse
import selectors
import subprocess
import sys
import time

parser = argparse.ArgumentParser()
parser.add_argument('elf')
parser.add_argument('--qemu', default='qemu-system-arm')
parser.add_argument('--timeout', type=float, default=60)
args = parser.parse_args()
command = [args.qemu, '-machine', 'lm3s6965evb', '-cpu', 'cortex-m3',
           '-nographic', '-net', 'none', '-icount', 'shift=6,align=off,sleep=off',
           '-rtc', 'clock=vm', '-kernel', args.elf]
proc = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
selector = selectors.DefaultSelector()
selector.register(proc.stdout, selectors.EVENT_READ)
output = bytearray()
result = 1
deadline = time.monotonic() + args.timeout
try:
    while time.monotonic() < deadline:
        events = selector.select(timeout=min(1, max(0, deadline - time.monotonic())))
        if not events:
            if proc.poll() is not None:
                break
            continue
        data = proc.stdout.read1(8192)
        if not data:
            break
        output.extend(data)
        sys.stdout.buffer.write(data)
        sys.stdout.buffer.flush()
        if b'PROJECT EXECUTION FAILED' in output or b'FATAL ERROR' in output:
            break
        if b'PROJECT EXECUTION SUCCESSFUL' in output:
            result = 0
            break
finally:
    selector.close()
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
if result:
    print('Missing successful ZTEST verdict (failure, exit or timeout).', file=sys.stderr)
sys.exit(result)
