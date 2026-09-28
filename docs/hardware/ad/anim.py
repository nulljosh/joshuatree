# Strata product film shots. Reuses the Strata geometry from concepts.py, renders
# each shot frame by frame (direct transforms, no keyframes) to shots/<name>/NNNN.png.
import bpy, math, sys, os
HW = "/Users/joshua/.claude/jobs/5b8457ff/tmp/hw"
FILM_OUT = sys.argv[sys.argv.index("--") + 1]
ONLY = sys.argv[sys.argv.index("--") + 2:] or None
src = open(os.path.join(HW, "concepts.py")).read()
sys.argv = ["x", "--", HW, "strata"]
exec(src[:src.index("def shot(")])  # ponytail: one geometry source, the renders and the film can't drift apart

scn.render.resolution_x, scn.render.resolution_y = 1920, 1080
scn.render.fps = 24
try: scn.eevee.taa_render_samples = 32
except Exception: pass

# everything that makes up the product hangs off one pivot for the turntable
pivot = bpy.data.objects.new("pivot", None); scn.collection.objects.link(pivot)
floor_names = {o.name for o in bpy.data.objects if o.type == "MESH" and o.dimensions.x > 50}
product = [o for o in bpy.data.objects if o.type in ("MESH", "CURVE") and o.name not in floor_names]
for o in product:
    o.parent = pivot
cap = bpy.data.objects["cap"]
H_top = max((o.matrix_world @ o.location).z for o in [cap])
on_cap = [o for o in product if o is not cap and not o.name.startswith(("stratum", "core", "shield", "usb", "hdmi", "eth", "io_", "ledline"))
          and o.location.z > cap.location.z - 0.01]
rings = [bpy.data.objects[f"stratum{i}"] for i in range(6)]
base_z = {o.name: o.location.z for o in rings + [cap] + on_cap}

# raking light, only for the engraving close-up
bpy.ops.object.light_add(type="AREA", location=(1.6, 0.25, 0.74))
rake = bpy.context.object; rake.data.energy = 160; rake.data.size = 0.35
rake.rotation_euler = (0, math.radians(92), math.radians(8)); rake.hide_render = True

cam_data = bpy.data.cameras.new("film"); cam = bpy.data.objects.new("film", cam_data)
scn.collection.objects.link(cam); scn.camera = cam
def aim(loc, look, lens):
    cam.location = loc; cam.data.lens = lens
    d = [look[i] - loc[i] for i in range(3)]
    cam.rotation_euler = (math.atan2(math.hypot(d[0], d[1]), -d[2]), 0, math.atan2(d[1], d[0]) - math.pi / 2)
def lerp(a, b, t): return tuple(a[i] + (b[i] - a[i]) * t for i in range(len(a)))
def ease(t): t = max(0.0, min(1.0, t)); return t * t * (3 - 2 * t)
def explode(k):
    for i, r in enumerate(rings): r.location.z = base_z[r.name] + k * i * 0.10
    for o in [cap] + on_cap: o.location.z = base_z[o.name] + k * 0.75

def s_hero(t):    aim(lerp((-4.4, -5.2, 2.5), (-3.3, -3.9, 1.85), ease(t)), (0, 0, 0.3), 55)
def s_turn(t):    pivot.rotation_euler.z = math.radians(25 - 60 * t); aim((0, -5.4, 1.75), (0, 0, 0.3), 62)
def s_explode(t): explode(ease(t / 0.8)); aim((-4.0, -4.7, 2.1), (0, 0, 0.6), 50)
def s_mark(t):    aim(lerp((-0.9, -1.45, 1.3), (0.3, -1.4, 1.25), ease(t)), (0, 0.06, 0.645), 55)
def s_end(t):     aim((-4.6, -5.8, 3.4), (0.3, 0.25, 0.45), 64)
SHOTS = [("hero", 60, s_hero), ("turn", 110, s_turn), ("explode", 70, s_explode), ("mark", 85, s_mark), ("end", 1, s_end)]

for name, n, fn in SHOTS:
    if ONLY and name not in ONLY: continue
    pivot.rotation_euler.z = 0; explode(0); rake.hide_render = name != "mark"
    d = os.path.join(FILM_OUT, name); os.makedirs(d, exist_ok=True)
    frames = [n // 2, n - 1] if os.environ.get('TESTFRAMES') else range(n)
    for f in frames:
        fn(f / max(1, n - 1))
        scn.render.filepath = os.path.join(d, f"{f:04d}.png")
        bpy.ops.render.render(write_still=True)
    print("SHOT", name, n)
print("ANIM DONE")
