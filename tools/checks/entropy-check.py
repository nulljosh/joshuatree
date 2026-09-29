#!/usr/bin/env python3
"""Entropy pool check (kernel/entropy.c), headless, one VM at a time.

Boots the kernel twice with the serial port on a file and QMP on a socket,
then asserts:
  1. Each boot logs "entropy: sources=..." naming at least jitter (QEMU's
     default CPU has no RDRAND, so jitter must carry the pool alone).
  2. entropy_boot_sample, the first 16 bytes the DRBG hands out at boot
     (the exact call auth_gen_salt makes for a password salt), differs
     between the two boots and is not all zeros. The old tick-seeded LCG
     produced the same salt on two identical boots; this is the regression
     it guards.
The full account story on top of these bytes (create, reboot, log in with
stored salt+hash) is tools/checks/auth-flow-check.py, which ci-suite.sh
already runs; it is the existing-login regression for this change.
"""
import pathlib, socket, subprocess, sys, time, json
from freeport import free_port

ROOT = pathlib.Path(__file__).resolve().parents[2]
ELF = ROOT / 'kernel.elf'
PORT = free_port()

def symbol(name):
    out = subprocess.run(['nm', str(ELF)], capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == name:
            return int(parts[0], 16) - 0xC0000000  # higher-half kernel: xp wants the physical address
    raise AssertionError(f'symbol {name} not in kernel.elf')

def qmp(sock, cmd, args=None):
    msg = {'execute': cmd}
    if args: msg['arguments'] = args
    sock.sendall((json.dumps(msg) + '\n').encode())
    buf = b''
    while b'\n' not in buf:
        buf += sock.recv(65536)
    return json.loads(buf.split(b'\n')[0])

def boot(n):
    log = pathlib.Path(f'/tmp/jt-entropy-{n}.log')
    if log.exists(): log.unlink()
    proc = subprocess.Popen(['qemu-system-i386', '-kernel', str(ELF), '-display', 'none', '-vga', 'std',
                             '-serial', f'file:{log}', '-qmp', f'tcp:127.0.0.1:{PORT},server,nowait'],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        sock = None
        for _ in range(50):
            try:
                sock = socket.create_connection(('127.0.0.1', PORT), timeout=1); break
            except OSError:
                time.sleep(.1)
        if sock is None: raise AssertionError('QMP never came up')
        sock.recv(65536); qmp(sock, 'qmp_capabilities')
        line = None
        for _ in range(100):
            text = log.read_text(errors='replace') if log.exists() else ''
            hit = [l for l in text.splitlines() if l.startswith('entropy: sources=')]
            if hit: line = hit[0]; break
            time.sleep(.1)
        if line is None: raise AssertionError('no "entropy: sources=" serial line within 10s')
        time.sleep(1)
        addr = symbol('entropy_boot_sample')
        r = qmp(sock, 'human-monitor-command', {'command-line': f'xp /16xb 0x{addr:x}'})['return']
        # hmp xp prints "ADDR: 0x12 0x34 ..." lines; take the bytes after each colon
        import re
        sample = bytes(int(v, 16) for line in r.splitlines() if ':' in line
                       for v in re.findall(r'0x([0-9a-f]{2})\b', line.split(':', 1)[1]))
        sock.close()
        return line, sample
    finally:
        proc.kill(); proc.wait()

fails = 0
def check(label, ok):
    global fails
    print(('ok   ' if ok else 'FAIL ') + label)
    if not ok: fails += 1

l1, s1 = boot(1)
l2, s2 = boot(2)
print(l1); print(l2); print('sample1', s1.hex()); print('sample2', s2.hex())
check('boot 1 serial names its entropy sources', 'jitter' in l1)
check('boot 2 serial names its entropy sources', 'jitter' in l2)
check('boot sample is 16 bytes', len(s1) == 16 and len(s2) == 16)
check('boot sample is not all zeros', s1 != bytes(16) and s2 != bytes(16))
check('two boots draw different first salts', s1 != s2)
if fails: print(f'FAIL: {fails} entropy checks failed'); sys.exit(1)
print('PASS: entropy sources logged, boot salts differ across two boots')
