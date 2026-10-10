#!/usr/bin/env python3
"""Brick physics under sanitizers, then real keyboard/mouse play and pixels in QEMU."""
import json
from pathlib import Path
import socket
import subprocess
import time
from PIL import Image
from scratch import scratch_dir

ROOT = Path(__file__).resolve().parents[2]
TMP = Path(scratch_dir("brick"))
source = r'''
#include <assert.h>
#include "arch/arm64/brick.h"
int main(void) {
    struct brick_game g;
    brick_reset(&g); assert(g.lives==3 && g.bricks==0xffffffffu && !g.state);
    brick_move(&g,-100); assert(g.paddle==38 && g.x==38);
    brick_move(&g,10000); assert(g.paddle==418 && g.x==418);
    brick_launch(&g); assert(g.state==1 && !g.paused);
    brick_launch(&g); int x=g.x,y=g.y; brick_step(&g); assert(g.x==x && g.y==y && g.paused);
    brick_launch(&g); assert(!g.paused);
    g.x=5; g.y=150; g.dx=-3; g.dy=-3; brick_step(&g); assert(g.dx==3 && g.x>=4);
    g.x=451; g.dx=3; brick_step(&g); assert(g.dx==-3 && g.x<=452);
    g.x=225; g.y=5; g.dy=-3; brick_step(&g); assert(g.dy==3 && g.y>=4);
    brick_reset(&g); g.state=1; g.x=g.paddle; g.y=216; g.dy=3;
    brick_step(&g); assert(g.dy<0 && g.y==218 && g.lives==3);
    brick_reset(&g); g.state=1; g.x=30; g.y=37; g.dx=0; g.dy=-3;
    brick_step(&g); assert(!(g.bricks&1) && g.score==1 && g.dy>0);
    g.bricks=1; g.score=31; g.x=30; g.y=37; g.dy=-3;
    brick_step(&g); assert(g.state==2 && g.score==32 && !g.bricks);
    brick_launch(&g); assert(g.state==1 && g.lives==3 && g.score==0 && g.bricks==0xffffffffu);
    for (int i=3;i>0;i--) {
        g.state=1; g.x=4; g.y=BRICK_H+BRICK_R; g.dx=0; g.dy=3;
        brick_step(&g); assert(g.lives==i-1 && g.state==(i==1?3:0));
    }
    brick_launch(&g); assert(g.state==1 && g.lives==3);
    for(int i=0;i<10000;i++) {
        if(g.state!=1) brick_launch(&g);
        brick_move(&g,g.x); brick_step(&g);
        assert(g.paddle>=38 && g.paddle<=418 && g.score>=0 && g.score<=32);
        assert(g.lives>=0 && g.lives<=3 && g.x>=4 && g.x<=452 && g.y>=4);
    }
}
'''
c=TMP/"physics.c"; c.write_text(source); exe=TMP/"physics"
subprocess.run(["clang","-Wall","-Wextra","-Werror","-fsanitize=address,undefined",
                "-I",str(ROOT),str(c),"-o",str(exe)],check=True)
subprocess.run([str(exe)],check=True,timeout=10)
print("PASS: wall/paddle/brick collisions, pause, win, three lives, restart and 10000-step bounds",flush=True)
subprocess.run(["make","-s","-C",str(ROOT/"arch/arm64"),"kernel8.elf"],check=True)
log=TMP/"uart"; sock=TMP/"qmp"
q=subprocess.Popen(["qemu-system-aarch64","-machine","virt","-cpu","cortex-a72","-m","256",
    "-nic","none","-global","virtio-mmio.force-legacy=false","-device","ramfb",
    "-device","virtio-keyboard-device","-device","virtio-tablet-device","-display","none",
    "-serial","file:"+str(log),"-qmp","unix:"+str(sock)+",server,nowait",
    "-kernel",str(ROOT/"arch/arm64/kernel8.elf")],stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
s=None; f=None
try:
    for _ in range(300):
        if log.exists() and "M2 input ready" in log.read_text(errors="replace"): break
        assert q.poll() is None,"QEMU exited"
        time.sleep(.1)
    else: raise AssertionError("desktop did not boot")
    s=socket.socket(socket.AF_UNIX); s.settimeout(10); s.connect(str(sock)); f=s.makefile("rw"); f.readline()
    def command(name,args=None):
        f.write(json.dumps({"execute":name,**({"arguments":args} if args else {})})+"\n");f.flush()
        while True:
            reply=json.loads(f.readline())
            assert "error" not in reply,reply
            if "return" in reply:return reply["return"]
    def key(name,hold=50):
        command("send-key",{"keys":[{"type":"qcode","data":name}],"hold-time":hold})
        time.sleep(hold/1000+.10)
    def shot(name):
        path=TMP/(name+".ppm");command("screendump",{"filename":str(path)})
        return Image.open(path).convert("RGB")
    def open_app(name):
        key("f1")
        for k in name:key(k)
        key("ret")
    command("qmp_capabilities")
    open_app("brick")
    assert "brick open" in log.read_text(),log.read_text()[-1000:]
    initial=shot("ready"); w,h=initial.size
    assert (w,h)==(800,600),(w,h)
    red=sum(1 for r,g,b in initial.crop((172,154,628,270)).getdata() if r>140 and 40<g<160 and b<130)
    assert red>20000,"brick wall missing"
    def paddle(image):
        xs=[x for x in range(172,628) if max(image.getpixel((x,378)))<70]
        assert len(xs)>=70,"paddle missing"
        return sum(xs)/len(xs)
    start=paddle(initial);key("left",250)
    assert paddle(shot("left"))<start-20,"held arrow did not move paddle"
    command("input-send-event",{"events":[{"type":"abs","data":{"axis":"x","value":int(260*32767/800)}},
        {"type":"abs","data":{"axis":"y","value":int(330*32767/600)}}]})
    time.sleep(.2)
    assert abs(paddle(shot("mouse"))-260)<5,"mouse did not move paddle"
    key("spc");a=shot("moving-a");time.sleep(.25);b=shot("moving-b")
    assert a.crop((172,154,628,394)).tobytes()!=b.crop((172,154,628,394)).tobytes(),"ball not animating without input"
    key("f1");overlay=shot("spotlight");time.sleep(.25)
    assert overlay.tobytes()==shot("spotlight-still").tobytes(),"game painted over Spotlight"
    key("esc")
    key("spc");paused=shot("paused");time.sleep(.25)
    assert paused.tobytes()==shot("still-paused").tobytes(),"pause kept animating"
    key("r");reset=shot("restart")
    assert abs(paddle(reset)-start)<2,"restart did not reset paddle"
    reset.save(TMP/"brick.png")
    def click():
        for down in [True,False]:
            command("input-send-event",{"events":[{"type":"btn","data":{"button":"left","down":down}}]})
            time.sleep(.1)
    plays=log.read_text().count("brick play")
    click();assert log.read_text().count("brick play")==plays+1,"mouse click did not launch"
    open_app("clock")
    uart=log.read_text();assert "brick closed" in uart and "clock open" in uart,"app switching failed"
    key("esc");open_app("brick")
    command("input-send-event",{"events":[{"type":"abs","data":{"axis":"x","value":int(174*32767/800)}},
        {"type":"abs","data":{"axis":"y","value":int(116*32767/600)}}]})
    time.sleep(.1);click()
    assert log.read_text().count("brick closed")>=2,"red close button did not close Brick"
    open_app("brick");key("esc")
    assert log.read_text().count("brick closed")>=3,"Escape did not close Brick"
    print("PASS: Spotlight launch, visible bricks, held keys, mouse, timed animation, pause, restart, app switching and Escape",flush=True)
finally:
    if f:f.close()
    if s:s.close()
    if q.poll() is None:q.terminate()
    try:q.wait(timeout=5)
    except subprocess.TimeoutExpired:q.kill();q.wait()
    q.stderr.close()
