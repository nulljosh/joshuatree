# The Joshua tree mark: four kinked arms, shaggy tufts. Settlers named the tree for Joshua raising his hands to the sky. Run from the repo root.
import math, sys
ink="#161513"
def build(arms, trunk, name, tuft_r=9.5):
    o=['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">', f'<path d="{trunk}" fill="{ink}"/>']
    for pts, w in arms:
        d="M"+" L".join(f"{x} {y}" for x,y in pts)
        o.append(f'<path d="{d}" fill="none" stroke="{ink}" stroke-width="{w}" stroke-linecap="round" stroke-linejoin="round"/>')
    for pts, w in arms:
        x,y=pts[-1]
        # pom-pom: spikes all around, longest upward, short drooping skirt below (dead leaves)
        for i in range(16):
            a=math.radians(i*22.5-90); up=-math.sin(a)
            r=tuft_r*(0.75+0.35*max(up,0)) if up>-0.3 else tuft_r*0.55
            o.append(f'<path d="M{x} {y}L{x+r*math.cos(a):.2f} {y-r*up:.2f}" stroke="{ink}" stroke-width="2.3" stroke-linecap="round"/>')
        o.append(f'<circle cx="{x}" cy="{y}" r="3.6" fill="{ink}"/>')
    o.append('</svg>'); open(name,"w").write("".join(o))
trunk="M44 96C45.5 84 46.5 72 47 62L54 62C54 72 54.5 84 56.5 96Z"
# j1: three arms, angular kinks, left low, center high, right mid

# j2: four arms, one forked, more shaggy classic silhouette
build([([(50,68),(36,64),(30,52),(20,48)],6), ([(49,62),(44,48),(36,40),(34,26)],6), ([(51,60),(56,44),(62,34),(60,20)],6), ([(51,66),(66,62),(74,50),(84,46)],6)], trunk, "landing/logo.svg", 8.5)
