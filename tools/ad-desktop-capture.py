import json, os, socket, subprocess, sys, time
from PIL import Image
REPO=os.path.dirname(os.path.dirname(os.path.abspath(__file__))); os.chdir(REPO); os.makedirs("/tmp/jt-loop/ad7cap",exist_ok=True)
OUT="/tmp/jt-loop/ad7cap"; LOG=OUT+"/serial.log"; DUMP=OUT+"/fb.raw"
FB=0xfd000000; W,H=1920,1080; PORT=4476; LW,LH,SC=960,540,2
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
    # Samantha opens alone, a single-window app; the cursor rests in the far corner; the corner and the dock tooltip are patched from the clean desktop below
    REST=(959,538)
    dock(7);move(*REST);time.sleep(1.2);frame("desktop_a")
    typ("Hi Samantha");time.sleep(0.5);move(*REST);time.sleep(0.8);frame("desktop_typed")
    key("ret");time.sleep(9);move(*REST);time.sleep(0.8);frame("desktop_b")
    clean=Image.open(OUT+"/desktop_clean.png").convert("RGB")
    for n in ("desktop_a","desktop_typed","desktop_b"):
        im=Image.open(f"{OUT}/{n}.png").convert("RGB")
        for box in ((1040,860,1230,916),(1860,1000,1920,1080)): im.paste(clean.crop(box),box[:2])
        im.save(f"{OUT}/{n}.png")
    print("ok")
finally:
    q.terminate()
    try:q.wait(timeout=5)
    except Exception:q.kill()
