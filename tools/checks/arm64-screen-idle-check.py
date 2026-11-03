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
static int accepted=1, busy, brick_live, input_ok=1;
__attribute__((unused)) static int input_present(void) { return input_ok; }
static unsigned notice_now(void) { return now; }
static int ask_pending(void) { return busy; }
static int screen_blank(unsigned blank) { calls++; last_blank=blank; return accepted; }
static void uart_puts(const char *s) { (void)s; }
static unsigned naps;
static void nap_ms(unsigned ms) { assert(ms == 20); naps++; }
/* 13.4.0: the Energy Saver's wait, and what its saver draws with */
static unsigned sleep_cs = 30000; unsigned settings_sleep_cs(void) { return sleep_cs; }
int saver_hold; static unsigned A[64], B[64], *fb, *fb_scan, fb_pitch = 8, fb_w = 8, fb_h = 8, flushes; static int text_ok;
static void fb_flush(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; if (!saver_hold) flushes++; }
static unsigned long cal_utc(void) { return 0; }
static void clock_local(unsigned long u, unsigned *a, unsigned *b, unsigned *c, unsigned *d) { (void)u; *a = *b = *c = *d = 0; }
static int sg(int v) { return v; } static unsigned fb_color(unsigned c) { return c; }
static int text_width(int w, const char *s, int p) { (void)w; (void)s; return p; }
static int text_draw(int w, const char *s, int x, int y, int p, unsigned c, unsigned *f, unsigned pi, int W, int H) { (void)w; (void)s; (void)x; (void)y; (void)p; (void)c; (void)f; (void)pi; (void)W; (void)H; return 0; }
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
    /* 7.5.3: nothing attached to wake it, so it never sleeps; once something is, a full five minutes count from then */
    screen_activity(); assert(calls==8 && !screen_asleep);
    input_ok=0; now=0x7520u+90000u; screen_tick(); assert(calls==8 && !screen_asleep);
    input_ok=1; now+=29999; screen_tick(); assert(calls==8);
    now+=1; screen_tick(); assert(calls==9 && screen_asleep);
    /* 10.1.0 deep idle: asleep and idle naps every pass; busy, or awake, never */
    screen_nap(0); screen_nap(0); assert(naps==2 && nap_count==2);
    screen_nap(1); assert(naps==2);
    screen_activity(); assert(!screen_asleep); screen_nap(0); assert(naps==2);
    /* 13.4.0 Energy Saver: a back buffer and a separate scanout get the saver first, on the scanout only, then dark */
    fb = A; fb_scan = B; for (int i = 0; i < 64; i++) A[i] = B[i] = 7;
    sleep_cs = 6000; now = 1000000; screen_activity(); unsigned c0 = calls;
    now += 5999; screen_tick(); assert(!saver_on && calls == c0);
    now += 1; screen_tick(); assert(saver_on && saver_hold && calls == c0 && !screen_asleep && B[0] == 0 && A[0] == 7);
    now += SAVER_CS - 1; screen_tick(); assert(saver_on && calls == c0);
    now += 1; screen_tick(); assert(calls == c0 + 1 && screen_asleep);
    unsigned f0 = flushes; screen_activity(); assert(!saver_on && !saver_hold && !screen_asleep && flushes == f0 + 1);
    sleep_cs = 0; now += 10000000; screen_tick(); assert(!saver_on && !screen_asleep);   /* Never */
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
assert host_check(header.replace(' || !input_present()', '')).returncode != 0, 'sleeps-with-no-input control passed'
assert host_check(header.replace('if (!screen_asleep || busy) return;', '(void)busy; if (!screen_asleep) return;')).returncode != 0, 'naps-while-busy control passed'
print('PASS: five-minute boundary, activity reset, wrap, active apps, refused requests, broken-wake control, and deep idle naps only asleep and idle',flush=True)
subprocess.run(['make','-s','-C',str(ROOT/'arch/arm64'),'screen-kernel8.img'],check=True)
log, sock = TMP/'uart', TMP/'qmp'
q = subprocess.Popen(['qemu-system-aarch64','-machine','raspi4b','-display','none',
    '-serial','null','-serial','file:'+str(log),'-qmp','unix:'+str(sock)+',server,nowait',
    '-kernel',str(ROOT/'arch/arm64/screen-kernel8.img')],stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
s = f = None
try:
    deadline = time.monotonic()+45
    while time.monotonic() < deadline:
        out = log.read_text(errors='replace') if log.exists() else ''
        if 'screen: deep idle selftest done' in out or 'screen: deep idle stuck' in out: break
        assert q.poll() is None, 'QEMU exited before the screen test'
        time.sleep(.1)
    else: raise AssertionError('screen selftest timed out: '+out[-600:])
    events = [line for line in out.splitlines() if line.startswith('screen:')]
    # 13.4.0 Energy Saver: the saver comes first, on the scanout, then the HDMI goes dark; a wake takes both back
    cycle = ['screen: saver on','screen: asleep','screen: saver off','screen: awake']
    assert events[:9] == cycle + cycle + ['screen: input selftest done'], events
    # 10.1.0: a second of the loop awake (spinning), then asleep (napping 20 ms a pass), then a key wakes it.
    # The nap waits on the counter, so the asleep count holds even where QEMU's wfe does not halt (10.x on CI).
    import re
    awake = int(re.search(r'screen: (\d+) passes in a second awake', out).group(1))
    asleep = int(re.search(r'screen: (\d+) passes in a second asleep', out).group(1))
    assert asleep <= 55, 'deep idle: %d passes in a second asleep, want about 50 (20 ms naps)' % asleep
    assert awake > 2 * asleep, 'awake (%d) should spin well past asleep (%d)' % (awake, asleep)
    assert 'screen: deep idle selftest done' in out, 'a key did not wake deep idle: ' + out[-400:]
    print('PASS: deep idle: %d loop passes a second awake, %d asleep, and a key wakes it' % (awake, asleep), flush=True)
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
