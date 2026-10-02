#!/usr/bin/env python3
"""Real-OS captures for the 2.0 ad, headless: boots kernel.elf under QEMU with no window, drives it over QMP
(Apps grid scrolled one step at a time, Samantha snapped left, Notes snapped right, "Hi Samantha" sent) and
saves the 1920x1080 framebuffer, the same pixels the OS draws at 2x, to /tmp/jt-ad-desktop. landing/ad/desktop-a.jpg
is desktop_a (before the line), desktop-b.jpg is desktop_b (line sent, her reply up), and the three icons with no
art/icons SVG (Portfolio, Activity, Clock) are cropped from apps_9. Needs `make kernel.elf` first."""
import json, os, socket, subprocess, sys, time
from PIL import Image
REPO=os.path.join(os.path.dirname(os.path.abspath(__file__)),".."); os.chdir(REPO)
OUT="/tmp/jt-ad-desktop"; os.makedirs(OUT,exist_ok=True); LOG=OUT+"/serial.log"; DUMP=OUT+"/fb.raw"
FB=0xfd000000; W,H=1920,1080; PORT=4471; LW,LH,SC=960,540,2
SLOT0_X,PITCH,ICON,DOCK_Y=247,43,37,487
q=subprocess.Popen(["qemu-system-i386","-kernel","kernel.elf","-display","none","-vga","std","-qmp",f"tcp:127.0.0.1:{PORT},server,nowait","-serial","file:"+LOG,"-net","nic,model=rtl8139","-net","user"],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
try:
    s=None
    for _ in range(100):
        time.sleep(0.2)
        try: s=socket.create_connection(("127.0.0.1",PORT));break
        except OSError: pass
    f=s.makefile("rw")
    def cmd(o):
        f.write(json.dumps(o)+"\n");f.flush()
        while True:
            r=json.loads(f.readline())
            if "return" in r or "error" in r: return r
    f.readline();cmd({"execute":"qmp_capabilities"})
    def move(x,y): cmd({"execute":"input-send-event","arguments":{"events":[{"type":"abs","data":{"axis":"x","value":int(x*32768/LW)}},{"type":"abs","data":{"axis":"y","value":int(y*32768/LH)}}]}})
    def btn(d): cmd({"execute":"input-send-event","arguments":{"events":[{"type":"btn","data":{"down":d,"button":"left"}}]}})
    def click(x,y):
        move(x,y);time.sleep(0.3);btn(True);time.sleep(0.1);btn(False);time.sleep(0.2)
    def key(k):
        cmd({"execute":"human-monitor-command","arguments":{"command-line":f"sendkey {k} 30"}});time.sleep(0.12)
    def typ(t):
        for ch in t:
            key("shift-"+ch.lower() if ch.isupper() else {" ":"spc",".":"dot",",":"comma","'":"apostrophe"}.get(ch,ch))
    def frame(name):
        cmd({"execute":"pmemsave","arguments":{"val":FB,"size":W*H*4,"filename":DUMP}})
        im=Image.frombytes("RGBA",(W,H),open(DUMP,"rb").read(),"raw","BGRA").convert("RGB");im.save(f"{OUT}/{name}.png");return im
    PARK=(480,200)
    def dock(slot):
        move(SLOT0_X+slot*PITCH+ICON//2,DOCK_Y);time.sleep(0.4)
        click_now()
        time.sleep(1.8);move(*PARK);time.sleep(0.5)
    def click_now():
        btn(True);time.sleep(0.1);btn(False)
    def at(x,y):
        move(x,y);time.sleep(0.3);click_now();time.sleep(0.8);move(*PARK);time.sleep(0.5)
    for _ in range(160):
        if frame("probe").getpixel((481*SC,512*SC))==(0xEF,0xEB,0xE4): break
        time.sleep(0.25)
    time.sleep(2.0)
    move(*PARK);time.sleep(0.5)
    frame("desktop_clean")
    dock(0)
    for i in range(10):
        frame(f"apps_{i}")
        cmd({"execute":"input-send-event","arguments":{"events":[{"type":"btn","data":{"down":True,"button":"wheel-down"}}]}});time.sleep(0.5)
    at(80,46)
    def drag(sx,sy,tx,ty):
        move(sx,sy);time.sleep(0.3);btn(True);time.sleep(0.15)
        for i in range(1,7):
            move(sx+(tx-sx)*i//6,sy+(ty-sy)*i//6);time.sleep(0.15)
        time.sleep(0.2);btn(False);time.sleep(0.8);move(*PARK);time.sleep(0.5)
    dock(7)
    drag(300,54,4,250)
    dock(4)
    drag(540,114,956,250);frame("two_open")
    at(240,400);move(940,500);time.sleep(0.4);frame("desktop_a")
    typ("Hi Samantha");time.sleep(0.5);move(940,500);time.sleep(0.4);frame("desktop_typed")
    key("ret");time.sleep(9);move(940,500);time.sleep(0.4);frame("desktop_b")
    print("ok")
finally:
    q.terminate()
    try:q.wait(timeout=5)
    except Exception:q.kill()
