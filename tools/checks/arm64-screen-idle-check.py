#!/usr/bin/env python3
"""Idle timing and real Pi mailbox/input routing. QEMU acknowledges blanking but does not hide pixels."""
import json
from pathlib import Path
import socket
import subprocess
import time
from scratch import scratch_dir

ROOT = Path(__file__).resolve().parents[2]
TMP = Path(scratch_dir('screen-idle'))
source = r'''
#include <assert.h>
static unsigned now, calls, last_blank;
static int accepted=1, busy, brick_live;
static unsigned notice_now(void) { return now; }
static int ask_pending(void) { return busy; }
static int screen_blank(unsigned blank) { calls++; last_blank=blank; return accepted; }
static void uart_puts(const char *s) { (void)s; }
#include "screen_idle.h"
int main(void) {
    screen_activity(); now=29999; screen_tick(); assert(!calls && !screen_asleep);
    now=30000; screen_tick(); assert(calls==1 && last_blank==1 && screen_asleep);
    now=60000; screen_tick(); assert(calls==1);
    accepted=0; screen_activity(); assert(calls==2 && !last_blank && screen_asleep);
    accepted=1; screen_activity(); assert(calls==3 && !screen_asleep);
    now=89999; screen_tick(); assert(calls==3);
    now=90000; accepted=0; screen_tick(); assert(calls==4 && !screen_asleep);
    now=90001; screen_tick(); assert(calls==4);
    now=120000; accepted=1; busy=1; screen_tick(); assert(calls==4);
    now=150000; busy=0; brick_live=1; screen_tick(); assert(calls==4);
    now=179999; brick_live=0; screen_tick(); assert(calls==4);
    now=180000; screen_tick(); assert(calls==5 && screen_asleep);
    now=0xfffffff0u; screen_activity(); assert(calls==6 && !screen_asleep);
    now=0x751fu; screen_tick(); assert(calls==6);
    now=0x7520u; screen_tick(); assert(calls==7 && screen_asleep);
}
'''
header = (ROOT/'arch/arm64/screen_idle.h').read_text()
c, exe = TMP/'idle.c', TMP/'idle'
c.write_text(source)
def host_check(text):
    (TMP/'screen_idle.h').write_text(text)
    subprocess.run(['clang','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(c),'-o',str(exe)],check=True)
    return subprocess.run([str(exe)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=10)
assert host_check(header).returncode == 0, 'idle/wake state check failed'
assert host_check(header.replace('screen_asleep = 0;', 'screen_asleep = 1;')).returncode != 0, 'broken-wake control passed'
print('PASS: five-minute boundary, activity reset, wrap, active apps, refused requests and broken-wake control',flush=True)
subprocess.run(['make','-s','-C',str(ROOT/'arch/arm64'),'screen-kernel8.img'],check=True)
log, sock = TMP/'uart', TMP/'qmp'
q = subprocess.Popen(['qemu-system-aarch64','-machine','raspi4b','-display','none',
    '-serial','file:'+str(log),'-qmp','unix:'+str(sock)+',server,nowait',
    '-kernel',str(ROOT/'arch/arm64/screen-kernel8.img')],stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
s = f = None
try:
    deadline = time.monotonic()+45
    while time.monotonic() < deadline:
        out = log.read_text(errors='replace') if log.exists() else ''
        if 'screen: input selftest done' in out: break
        assert q.poll() is None, 'QEMU exited before the screen test'
        time.sleep(.1)
    else: raise AssertionError('screen selftest timed out: '+out[-600:])
    events = [line for line in out.splitlines() if line.startswith('screen:')]
    assert events == ['screen: asleep','screen: awake','screen: asleep','screen: awake','screen: input selftest done'], events
    assert 'key 42 down' in out and 'key 42 up' in out and 'mouse ' in out, 'wake swallowed input'
    s = socket.socket(socket.AF_UNIX); s.settimeout(10); s.connect(str(sock)); f=s.makefile('rw'); f.readline()
    def command(name, args=None):
        f.write(json.dumps({'execute':name,**({'arguments':args} if args else {})})+'\n'); f.flush()
        while True:
            result=json.loads(f.readline()); assert 'error' not in result, result
            if 'return' in result: return
    command('qmp_capabilities')
    command('screendump',{'filename':str(TMP/'awake.ppm')})
    print('PASS: raspi4b mailbox blank/unblank, keyboard and mouse wake; input continues. Physical HDMI blanking needs the board.',flush=True)
finally:
    if f: f.close()
    if s: s.close()
    q.terminate()
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill(); q.wait()
    q.stderr.close()
