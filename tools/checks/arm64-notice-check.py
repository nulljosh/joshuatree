#!/usr/bin/env python3
"""Bounded menu notices, wrap-safe expiry, real pixels and uninterrupted input."""
import json
from pathlib import Path
import socket
import subprocess
import time
from PIL import Image, ImageChops
from scratch import scratch_dir

ROOT = Path(__file__).resolve().parents[2]
TMP = Path(scratch_dir('notice'))
c = TMP/'notice.c'
c.write_text(r'''
#include <assert.h>
#include <string.h>
#include "arch/arm64/notice.h"
int main(void) {
    struct menu_notice n = {0};
    assert(!notice_expire(&n, 500));
    notice_set(&n, "First", 100);
    assert(!notice_expire(&n, 499) && !strcmp(n.text,"First"));
    notice_set(&n, "New", 499);
    assert(!notice_expire(&n, 898) && !strcmp(n.text,"New"));
    assert(notice_expire(&n, 899) && !n.text[0]);
    assert(!notice_expire(&n, 900));
    notice_set(&n, "Wrap", 0xffffff00u);
    assert(!notice_expire(&n, 0x8fu)); assert(notice_expire(&n, 0x90u));
    char long_message[100]; memset(long_message,'x',sizeof long_message);
    notice_set(&n,long_message,0); assert(strlen(n.text)==47);
    notice_set(&n,"a\nb\x01\xff",0); assert(!strcmp(n.text,"a?b??"));
    notice_set(&n,0,0); assert(!n.text[0]);
}
''')
exe = TMP/'notice'
subprocess.run(['clang','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I',str(ROOT),str(c),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True,timeout=10)
print('PASS: bounds, replacement, sanitizing, empty notices and four-second expiry across wrap',flush=True)
subprocess.run(['make','-s','-C',str(ROOT/'arch/arm64'),'notice-kernel8.elf'],check=True)
log, sock = TMP/'uart', TMP/'qmp'
q = subprocess.Popen(['qemu-system-aarch64','-machine','virt','-cpu','cortex-a72','-m','256',
    '-nic','none','-global','virtio-mmio.force-legacy=false','-device','ramfb',
    '-device','virtio-keyboard-device','-display','none','-serial','file:'+str(log),
    '-qmp','unix:'+str(sock)+',server,nowait','-kernel',str(ROOT/'arch/arm64/notice-kernel8.elf')],
    stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
s = f = None
try:
    for _ in range(300):
        if log.exists() and 'notice: Note kept in memory' in log.read_text(errors='replace'): break
        assert q.poll() is None, 'QEMU exited'
        time.sleep(.1)
    else: raise AssertionError('notice boot did not finish')
    s = socket.socket(socket.AF_UNIX); s.settimeout(10); s.connect(str(sock)); f=s.makefile('rw');f.readline()
    def command(name, args=None):
        f.write(json.dumps({'execute':name,**({'arguments':args} if args else {})})+'\n');f.flush()
        while True:
            r=json.loads(f.readline());assert 'error' not in r,r
            if 'return' in r:return r['return']
    def shot(name):
        p=TMP/(name+'.ppm');command('screendump',{'filename':str(p)});return Image.open(p).convert('RGB')
    def key(keys):
        command('send-key',{'keys':[{'type':'qcode','data':k} for k in keys],'hold-time':50});time.sleep(.15)
    command('qmp_capabilities')
    shown=shot('shown');w,h=shown.size
    key(['ctrl','spc']);key(['c']);key(['esc'])
    assert 'spotlight open' in log.read_text(errors='replace'), 'notice stole input'
    for _ in range(65):
        if 'notice cleared' in log.read_text(errors='replace'): break
        time.sleep(.1)
    else: raise AssertionError('notice never expired without input')
    clear=shot('clear');bar=h*26//540
    roi=(w//2,0,w-150*h//540,bar)
    changed=ImageChops.difference(shown.crop(roi),clear.crop(roi))
    assert changed.getbbox(), 'notice did not draw real text'
    assert shown.crop((w-140*h//540,0,w,bar)).tobytes()==clear.crop((w-140*h//540,0,w,bar)).tobytes(), 'notice damaged clock or Wi-Fi'
    assert 'notice cleared' in log.read_text(errors='replace')
    shown.save(TMP/'notification.png')
    print('PASS: visible notice clears without input; Spotlight works; clock and Wi-Fi pixels stay intact',flush=True)
finally:
    if f:f.close()
    if s:s.close()
    q.terminate()
    try:q.wait(timeout=5)
    except subprocess.TimeoutExpired:q.kill();q.wait()
