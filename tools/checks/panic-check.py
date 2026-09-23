#!/usr/bin/env python3
"""Headless proof that a ring-0 exception paints a readable panic screen over
the running desktop instead of a silent freeze: boots to the desktop, opens
Terminal from the dock, types the shell's own `crash` (int $3), waits for
"exception: ring-0" on serial, then checks the frame is the cream panic fill,
the dock is gone and there is ink where the reason is drawn.
Usage: tools/checks/panic-check.py   (from the repo root, after make kernel.elf)
"""
import json,socket,subprocess,time,sys
from PIL import Image
port=4651; dump='/tmp/jt-panic.raw'; log='/tmp/jt-panic-serial.log'
q=subprocess.Popen(["qemu-system-i386","-name","jt-panictest","-kernel","kernel.elf","-display","none","-vga","std","-no-reboot","-serial","file:"+log,"-qmp","tcp:127.0.0.1:%d,server,nowait"%port],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
time.sleep(0.5); s=socket.create_connection(("127.0.0.1",port)); f=s.makefile("rw"); f.readline()
def cmd(o):
    f.write(json.dumps(o)+"\n"); f.flush()
    while True:
        r=json.loads(f.readline())
        if "return" in r or "error" in r: return r
def shot(path):
    cmd({"execute":"pmemsave","arguments":{"val":0xfd000000,"size":1920*1080*4,"filename":dump}})
    im=Image.frombytes("RGBA",(1920,1080),open(dump,"rb").read(),"raw","BGRA").convert("RGB"); im.save(path); return im
def move(x,y): cmd({"execute":"input-send-event","arguments":{"events":[{"type":"abs","data":{"axis":"x","value":x*32768//960}},{"type":"abs","data":{"axis":"y","value":y*32768//540}}]}})
def click():
    for d in (True,False): cmd({"execute":"input-send-event","arguments":{"events":[{"type":"btn","data":{"down":d,"button":"left"}}]}}); time.sleep(0.1)
def key(*ks): cmd({"execute":"send-key","arguments":{"keys":[{"type":"qcode","data":k} for k in ks]}}); time.sleep(0.15)
cmd({"execute":"qmp_capabilities"})
for _ in range(100):
    if shot('/tmp/jt-panic0.png').getpixel((961,1023))==(0xEF,0xEB,0xE4): break
    time.sleep(0.2)
time.sleep(1); move(247+6*43+18,487); time.sleep(0.3); click(); time.sleep(1.5)
for c in "crash": key(c)
key("ret")
for _ in range(50):
    time.sleep(0.2)
    if "exception: ring-0" in open(log,errors="replace").read(): break
time.sleep(1.0); shot('/tmp/jt-panic.png')
serial=open(log,errors="replace").read()
im=Image.open('/tmp/jt-panic.png'); px=list(im.getdata()); n=len(px)
cream=sum(1 for p in px if p==(0xFA,0xF8,0xF6))/n
dock=im.getpixel((961,1023))==(0xEF,0xEB,0xE4)
ink=sum(1 for y in range(200,420) for x in range(40,700) if im.getpixel((x,y))!=(0xFA,0xF8,0xF6))
fails=[]
if "exception: ring-0" not in serial: fails.append("no ring-0 exception on serial")
if cream<0.9: fails.append("panic screen covers only %.0f%% of the frame"%(cream*100))
if dock: fails.append("dock tray still visible under the panic screen")
if ink<200: fails.append("no text where the exception name and lines should be")
print("cream %.1f%% dock=%s ink=%d"%(cream*100,dock,ink))
print("FAIL: "+"; ".join(fails) if fails else "PASS: a ring-0 fault paints a full-screen panic with the reason, over the desktop")
rc=1 if fails else 0
try: cmd({"execute":"quit"})
except Exception: pass
q.wait(timeout=5)
sys.exit(rc)
