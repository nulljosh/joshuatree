import http.server, json, os, socket, subprocess, sys, threading, time
ROOT="/tmp/jt-loop/reset"; os.chdir(ROOT)
sys.path.insert(0, ROOT+"/tools/checks")
from freeport import free_port
QP=free_port(); mem=sys.argv[1] if len(sys.argv)>1 else "64"
class H(http.server.BaseHTTPRequestHandler):
    def log_message(s,*a): open("/tmp/jt-loop/req.log","a").write(s.path+" "+str(a)+"\n")
    def rep(s,c,b,t):
        s.send_response(c); s.send_header("Content-Type",t); s.send_header("Content-Length",str(len(b))); s.end_headers(); s.wfile.write(b)
    def do_GET(s):
        p=ROOT+"/landing"+s.path
        if os.path.isfile(p): s.rep(200,open(p,"rb").read(),"image/jpeg")
        else: s.rep(404,b"nf","text/plain")
    def do_POST(s):
        s.rfile.read(int(s.headers.get("Content-Length","0")))
        if s.path=="/api/chat": s.rep(200,json.dumps({"model":"samantha","message":{"role":"assistant","content":"Paris."},"done":True}).encode(),"application/json")
        elif s.path=="/api/pick": s.rep(200,b'{"tool":""}',"application/json")
        else: s.rep(404,b"nf","text/plain")
srv=http.server.ThreadingHTTPServer(("127.0.0.1",0),H); srv.daemon_threads=True
threading.Thread(target=srv.serve_forever,daemon=True).start(); port=srv.server_address[1]
log="/tmp/jt-loop/reset/serial.txt"; ql="/tmp/jt-loop/reset/qemu.log"
for f in (log,ql):
    try: os.remove(f)
    except: pass
q=subprocess.Popen(["qemu-system-i386","-kernel","kernel.elf","-m",mem,"-display","none","-vga","std","-no-reboot",
 "-qmp",f"tcp:127.0.0.1:{QP},server,nowait","-serial","file:"+log,"-net","nic,model=rtl8139","-net","user",
 "-d","int,cpu_reset,guest_errors","-D",ql,
 "-append",f"samantha llmhost=10.0.2.2 llmport={port} facehost=10.0.2.2:{port}"],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
def ser():
    try: return open(log,errors="replace").read()
    except: return ""
try:
    s=None
    for _ in range(50):
        time.sleep(0.2)
        try: s=socket.create_connection(("127.0.0.1",QP)); break
        except OSError: pass
    f=s.makefile("rw")
    def cmd(o):
        f.write(json.dumps(o)+"\n"); f.flush()
        while True:
            r=json.loads(f.readline())
            if "return" in r or "error" in r: return r
    f.readline(); cmd({"execute":"qmp_capabilities"})
    t=time.time()+120
    while time.time()<t and "face: idle" not in ser() and q.poll() is None: time.sleep(0.3)
    print("face line:", [l for l in ser().splitlines() if "face: " in l], "boots", ser().count("kmain boot start"))
    time.sleep(1)
    for ch in "what is the capital of france":
        cmd({"execute":"send-key","arguments":{"keys":[{"type":"qcode","data":"spc" if ch==" " else ch}]}}); time.sleep(0.15)
        if q.poll() is not None or ser().count("kmain boot start")>1: print("DIED after",repr(ch)); break
    time.sleep(1)
    cmd({"execute":"send-key","arguments":{"keys":[{"type":"qcode","data":"ret"}]}}); time.sleep(8)
    print("boots",ser().count("kmain boot start"),"qemu alive",q.poll() is None)
finally:
    q.kill(); q.wait()
