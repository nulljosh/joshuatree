# The Joshua tree mark: settlers named the tree for Joshua raising his hands to the sky. Run from the repo root.
import math
ink="#161513"
o=['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">']
# tapered trunk, wider at the root
o.append(f'<path d="M43.5 95C45 82 46.5 70 47.5 60L53.5 60C53 70 53.5 82 55.5 95Z" fill="{ink}"/>')
# arms as curved round-capped strokes
o.append(f'<path d="M49 67C40 66 32 60 29 42" fill="none" stroke="{ink}" stroke-width="7" stroke-linecap="round"/>')
o.append(f'<path d="M51 62C58 55 63 42 64 22" fill="none" stroke="{ink}" stroke-width="7" stroke-linecap="round"/>')
def tuft(x,y,r,n=9,spread=22):
    for i in range(n):
        a=math.radians(-90+(i-(n-1)/2)*spread)
        o.append(f'<path d="M{x} {y}L{x+r*math.cos(a):.2f} {y+r*math.sin(a):.2f}" stroke="{ink}" stroke-width="2.8" stroke-linecap="round"/>')
tuft(29,40,10.5); tuft(64,20,11.5)
o.append('</svg>')
open("landing/logo.svg","w").write("".join(o))
