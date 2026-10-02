# Strata Kit product renders, from the real CAD. One script, three views.
#   python3 docs/hardware/render_strata.py [outdir]     (default docs/hardware/render)
# Step 1 re-runs strata_cad.py (all assertions, STLs, STEP, manifest) and has it write assembled-position meshes.
# Step 2 runs Blender headless on those meshes: cream ground, soft light, hero / rear / top views, 1600 px wide PNG.
# Every printed piece (24 ring quarters, cap panel and frame, tray, rear plate, feet) is loaded in its assembled position with the 0.2 mm seams, in the filament colour strata_cad.py wrote to stl/manifest.json. No metal paint, no gloss, no text.
import sys, os, subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
PREVIEW = bool(os.environ.get("STRATA_PREVIEW"))             # 400 px, 8 samples, for checking exposure
W, H = (400, 250) if PREVIEW else (1600, 1000)
BASE, TOPC = (0xB9, 0x54, 0x2C), (0xF0, 0xE7, 0xD8)      # ring 0 and the cap, as in ASSEMBLY.md
INK, CREAM = (0x1E, 0x1C, 0x1A), (0xE9, 0xE0, 0xCF)
BLOCK = (127.0, 127.0, 50.0)                                # plain reference block, mm

try:
    import bpy
except ImportError:
    bpy = None

def check(out):
    """fails if a lit front face is more than 8 degrees of hue or 12 L* off its manifest hex, or the black rear-plate strip is brighter than 60"""
    import json, colorsys, re
    proj = json.load(open(os.path.join(out, "proj.json"))); man = {m["part"]: m["hex"] for m in json.load(open(os.path.join(HERE, "stl", "manifest.json")))}
    def px(img, x, y):
        t = subprocess.run(["magick", os.path.join(out, img), "-crop", "5x1+%d+%d" % (x - 2, y), "+repage", "-scale", "1x1!", "-depth", "8", "txt:-"], capture_output=True, text=True).stdout
        return tuple(int(v) for v in re.search(r"\((\d+),(\d+),(\d+)", t.split("\n")[1] if "\n" in t else t).groups())
    def lstar(c):
        lin = [(v / 255 / 12.92) if v / 255 <= 0.04045 else ((v / 255 + 0.055) / 1.055) ** 2.4 for v in c]; Y = 0.2126 * lin[0] + 0.7152 * lin[1] + 0.0722 * lin[2]
        return 116 * (Y ** (1 / 3)) - 16 if Y > 0.008856 else 903.3 * Y
    hue = lambda c: colorsys.rgb_to_hsv(*[v / 255 for v in c])[0] * 360
    bad = []
    for key, part in (("ring0", "ring0_front"), ("ring5", "ring5_front"), ("cap", "cap_frame_front")):
        x, y = proj["hero:" + key]; got = px("hero.png", x, y); h = man[part].lstrip("#"); want = tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))
        dh = abs((hue(got) - hue(want) + 180) % 360 - 180) if min(max(got) - min(got), max(want) - min(want)) > 12 else 0.0
        dl = abs(lstar(got) - lstar(want)); print("CHECK %-5s got %s want %s hue %.1f deg L* %.1f" % (key, got, want, dh, dl))
        if dh > 8 or dl > 12: bad.append(key)
    gs = [px("rear.png", *proj["rear:strip_bottom"]), px("rear.png", *proj["rear:strip_top"])]; m = sum(sum(g) / 3 for g in gs) / 2
    print("CHECK rear plate strip mean %.0f (limit 60)" % m, gs, "ground hero", px("hero.png", *proj["hero:ground"]), "rear", px("rear.png", *proj["rear:ground"]))
    if m > 60: bad.append("rear strip")
    if bad: print("FAIL", bad); return 1
    print("PASS render colour check"); return 0

if bpy is None:
    out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "render"))
    mesh = os.path.join(out, "mesh"); os.makedirs(mesh, exist_ok=True)
    if os.environ.get("STRATA_CHECK_ONLY"): sys.exit(check(out))      # re-run just the colour check on existing renders
    env = dict(os.environ, STRATA_MESH=mesh)
    subprocess.run(["uv", "run", "--with", "build123d", "--with", "shapely", "python", os.path.join(HERE, "strata_cad.py"), HERE], check=True, env=env)
    subprocess.run(["/opt/homebrew/bin/blender", "-b", "-P", __file__, "--", mesh, out], check=True)
    sys.exit(check(out))

import math
from mathutils import Vector
mesh_dir, out_dir = sys.argv[sys.argv.index("--") + 1:][:2]
S = 0.001                                                   # mm to metres

def lin(c):
    f = lambda v: (v / 255.0 / 12.92) if v / 255.0 <= 0.04045 else (((v / 255.0) + 0.055) / 1.055) ** 2.4
    return (f(c[0]), f(c[1]), f(c[2]), 1.0)
def mix(a, b, t): return tuple(round(a[i] + (b[i] - a[i]) * t) for i in range(3))

bpy.ops.wm.read_factory_settings(use_empty=True)
sc = bpy.context.scene
sc.render.engine = "CYCLES"
sc.cycles.samples = 16 if PREVIEW else 256
SAMPLES_REAR = 16 if PREVIEW else 640                      # the rear window reveal shows denoiser smudge below this
sc.cycles.use_denoising = True
sc.cycles.max_bounces = 6
sc.render.resolution_x, sc.render.resolution_y, sc.render.resolution_percentage = W, H, 100
sc.view_settings.view_transform = "Standard"                # filament colours must come out as the manifest hex, so no tone-mapper hue shifts
try: sc.view_settings.look = "None"
except TypeError: pass
sc.view_settings.exposure = 0.0
try:
    prefs = bpy.context.preferences.addons["cycles"].preferences
    prefs.compute_device_type = "METAL"; prefs.get_devices()
    for d in prefs.devices: d.use = True
    sc.cycles.device = "GPU"
except Exception:
    sc.cycles.device = "CPU"

def material(name, rgb, rough=0.55):
    m = bpy.data.materials.new(name); m.use_nodes = True
    b = next(n for n in m.node_tree.nodes if n.type == "BSDF_PRINCIPLED")
    b.inputs["Base Color"].default_value = lin(rgb); b.inputs["Roughness"].default_value = rough
    b.inputs["Metallic"].default_value = 0.0
    return m

def load(name, mat):
    p = os.path.join(mesh_dir, name + ".stl")
    try: bpy.ops.wm.stl_import(filepath=p)
    except AttributeError: bpy.ops.import_mesh.stl(filepath=p)
    o = bpy.context.selected_objects[0]; o.name = name
    o.scale = (S, S, S); bpy.context.view_layer.objects.active = o; o.select_set(True)
    bpy.ops.object.transform_apply(scale=True)
    o.data.materials.append(mat)
    for poly in o.data.polygons: poly.use_smooth = True
    mod = o.modifiers.new("edges", "EDGE_SPLIT"); mod.use_edge_angle = True; mod.split_angle = math.radians(30)
    return o

# the machine: every printed piece in its own filament colour (from the CAD), rods hidden inside the rings
import json
for m in json.load(open(os.path.join(mesh_dir, "meshes.json"))):
    h = m["hex"].lstrip("#"); rgb = tuple(min(int(h[i:i + 2], 16), 238) for i in (0, 2, 4))   # no PLA reflects 100 percent: white filament renders at 238 so it never clips
    load(m["name"], material(m["name"], rgb, 0.6))

# stand-in stock I/O shield (158.75 x 44.45) seated at the board's rear edge, y = 88, with the 12 real J4125B-ITX port profiles cut through it by a boolean.
# Seen from behind, left to right: PS/2, 2x USB 2.0 stack, VGA, DVI-D, HDMI, 2x USB 3.2 stack, RJ45, three audio jacks. x here is mirrored for that (the camera looks down -y). Ports sit on the PCB at z 9.
# The shield is the board's own steel part, so brushed metal is honest here (unlike the printed rings).
IO_Z = 29.0; PZ = 9.0
import bmesh
def prism(name, pts, y0, y1):
    bm = bmesh.new(); f0 = [bm.verts.new((x * S, y0 * S, z * S)) for x, z in pts]; f1 = [bm.verts.new((x * S, y1 * S, z * S)) for x, z in pts]
    bm.faces.new(f0); bm.faces.new(f1[::-1])
    for i in range(len(pts)): bm.faces.new((f0[i], f0[(i + 1) % len(pts)], f1[(i + 1) % len(pts)], f1[i]))
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
    me = bpy.data.meshes.new(name); bm.to_mesh(me); bm.free(); o = bpy.data.objects.new(name, me); sc.collection.objects.link(o); return o
def rect(x0, x1, z0, z1): return [(x0, z0), (x1, z0), (x1, z1), (x0, z1)]
def trap(x0, x1, z0, z1, ins): return [(x0 + ins, z0), (x1 - ins, z0), (x1, z1), (x0, z1)]       # D-sub and HDMI: wide at the top
def circ(cx, cz, r, n=24): return [(cx + r * math.cos(2 * math.pi * i / n), cz + r * math.sin(2 * math.pi * i / n)) for i in range(n)]
def rj45(x0, x1, z0, z1):
    cx = (x0 + x1) / 2; return [(x0, z0), (x1, z0), (x1, z1), (cx + 3, z1), (cx + 3, z1 + 1.6), (cx - 3, z1 + 1.6), (cx - 3, z1), (x0, z1)]
M = lambda x0, x1: (-x1, -x0)                                   # mirror x for the view from behind
PORTS = [("ps2", circ(71.3, PZ + 5.5, 5.5), None),
         ("usb2a", rect(*M(-62.3, -49.3)[:2], PZ, PZ + 5.7), "usb"), ("usb2b", rect(*M(-62.3, -49.3)[:2], PZ + 8.5, PZ + 14.2), "usb"),
         ("vga", trap(*M(-45.8, -17.8)[:2], PZ, PZ + 12.5, 1.6), None),
         ("dvi", trap(*M(-14.3, 13.7)[:2], PZ, PZ + 12.5, 1.6), None),
         ("hdmi", trap(*M(17.2, 31.2)[:2], PZ, PZ + 6, 1.3), None),
         ("usb3a", rect(*M(34.7, 47.7)[:2], PZ, PZ + 5.7), "usb"), ("usb3b", rect(*M(34.7, 47.7)[:2], PZ + 8.5, PZ + 14.2), "usb"),
         ("rj45", rj45(*M(51.2, 66.2)[:2], PZ, PZ + 13.5), None),
         ("a1", circ(-73.2, PZ + 3.5, 3.4), None), ("a2", circ(-73.2, PZ + 11.7, 3.4), None), ("a3", circ(-73.2, PZ + 19.9, 3.4), None)]
bpy.ops.mesh.primitive_cube_add(size=1)
shd = bpy.context.object; shd.name = "shield"; shd.scale = (158.75 * S, 0.8 * S, 44.45 * S); shd.location = (0, 88.4 * S, IO_Z * S)
bpy.ops.object.transform_apply(scale=True)
cut = [prism("cut_" + n, pts, 87.0, 89.8) for n, pts, _k in PORTS]
bpy.ops.object.select_all(action='DESELECT')
for c in cut: c.select_set(True)
bpy.context.view_layer.objects.active = cut[0]; bpy.ops.object.join(); cutter = bpy.context.object
bm_ = shd.modifiers.new("ports", "BOOLEAN"); bm_.operation = "DIFFERENCE"; bm_.object = cutter; bm_.solver = "EXACT"
bpy.context.view_layer.objects.active = shd; bpy.ops.object.modifier_apply(modifier="ports"); bpy.data.objects.remove(cutter)
steel = material("shield", (170, 168, 163), 0.4)
for n_ in steel.node_tree.nodes:
    if n_.type == "BSDF_PRINCIPLED": n_.inputs["Metallic"].default_value = 1.0
shd.data.materials.append(steel)
for pl in shd.data.polygons: pl.use_smooth = False
# dark connector body 3 mm behind each opening and 5 mm deep, lighter tongue in the USB ports
body_m = material("conn", (24, 22, 21), 0.7); tongue_m = material("tongue", (214, 210, 200), 0.5)
for n, pts, kind in PORTS:
    xs = [p[0] for p in pts]; zs = [p[1] for p in pts]; x0, x1, z0, z1 = min(xs) + 0.5, max(xs) - 0.5, min(zs) + 0.5, min(zs) + (max(zs) - min(zs)) - 0.5
    if n == "rj45": z1 -= 1.6
    bpy.ops.mesh.primitive_cube_add(size=1); p = bpy.context.object
    p.scale = ((x1 - x0) * S, 5 * S, (z1 - z0) * S); p.location = ((x0 + x1) / 2 * S, (88.0 - 3 - 2.5) * S, (z0 + z1) / 2 * S)
    bpy.ops.object.transform_apply(scale=True); p.data.materials.append(body_m)
    if kind == "usb":
        bpy.ops.mesh.primitive_cube_add(size=1); t = bpy.context.object
        t.scale = ((x1 - x0 - 3) * S, 4.4 * S, 1.4 * S); t.location = ((x0 + x1) / 2 * S, (88.0 - 3 + 0.2) * S, (z0 + z1) / 2 * S)
        bpy.ops.object.transform_apply(scale=True); t.data.materials.append(tongue_m)

# reference block, 127 x 127 x 50 mm, plain matte grey, uniform 4 mm radius, no marks
bpy.ops.mesh.primitive_cube_add(size=1)
blk = bpy.context.object; blk.name = "block"
blk.scale = (BLOCK[0] * S, BLOCK[1] * S, BLOCK[2] * S); blk.location = (205 * S, -25 * S, BLOCK[2] / 2 * S)
bpy.ops.object.transform_apply(scale=True)
bv = blk.modifiers.new("bevel", "BEVEL"); bv.width = 4 * S; bv.segments = 6; bv.limit_method = "NONE"
blk.data.materials.append(material("block", (176, 172, 165), 0.5))
for poly in blk.data.polygons: poly.use_smooth = True

# curved cream sweep: a bowl of floor, fillet and wall, so there is no horizon line
import bmesh
prof = [(0.0, 0.0), (2.2, 0.0)] + [(2.2 + 1.2 * math.sin(t), 1.2 - 1.2 * math.cos(t)) for t in [i * math.pi / 2 / 12 for i in range(1, 13)]] + [(3.4, 5.0)]
bm = bmesh.new(); rings_v = []
for (r, zz) in prof:
    rings_v.append([bm.verts.new((r * math.cos(th), r * math.sin(th), zz)) for th in [i * 2 * math.pi / 96 for i in range(96)]])
for i in range(len(prof) - 1):
    for j in range(96):
        bm.faces.new((rings_v[i][j], rings_v[i][(j + 1) % 96], rings_v[i + 1][(j + 1) % 96], rings_v[i + 1][j]))
bm.normal_update(); me = bpy.data.meshes.new("sweep"); bm.to_mesh(me); gnd = bpy.data.objects.new("sweep", me); sc.collection.objects.link(gnd)
for pl in me.polygons: pl.use_smooth = True
gnd.data.materials.append(material("ground", CREAM, 0.9))
sc.world = bpy.data.worlds.new("w"); sc.world.use_nodes = True
bg = next(n for n in sc.world.node_tree.nodes if n.type == "BACKGROUND")
bg.inputs["Color"].default_value = lin(CREAM); bg.inputs["Strength"].default_value = 0.12

def area(name, loc, size, power, target=(0, 0, 0.02)):
    d = bpy.data.lights.new(name, "AREA"); d.size = size; d.energy = power; d.color = (1.0, 0.97, 0.92)
    if name.startswith("wall"): d.specular_factor = 0.0                 # the backdrop lights only light the sweep, they never mirror in the product
    o = bpy.data.objects.new(name, d); sc.collection.objects.link(o); o.location = loc
    o.rotation_euler = (Vector(target) - Vector(loc)).to_track_quat("-Z", "Y").to_euler()
    return o

cam_d = bpy.data.cameras.new("cam"); cam = bpy.data.objects.new("cam", cam_d); sc.collection.objects.link(cam); sc.camera = cam
cam_d.sensor_width = 36; cam_d.clip_start = 0.01; cam_d.clip_end = 50

PROJ = {}                                                     # pixel positions of the check points, written for the post-render check
def shoot(name, az, el, dist, lens, target, lights, ev=0.0, samples=None, pts=None):
    sc.view_settings.exposure = ev
    sc.cycles.samples = samples or sc.cycles.samples
    for o in [o for o in sc.objects if o.type == "LIGHT"]: bpy.data.objects.remove(o)
    for l in lights: area(*l)
    a, e = math.radians(az), math.radians(el)
    t = Vector([v * S for v in target])
    cam.location = t + Vector((math.sin(a) * math.cos(e), -math.cos(a) * math.cos(e), math.sin(e))) * dist
    cam.rotation_euler = (t - cam.location).to_track_quat("-Z", "Y").to_euler()
    cam_d.lens = lens
    bpy.context.view_layer.update()
    from bpy_extras.object_utils import world_to_camera_view
    for k, p in (pts or {}).items():
        v = world_to_camera_view(sc, cam, Vector([c * S for c in p])); PROJ[name + ":" + k] = (round(v.x * W), round((1 - v.y) * H))
    sc.render.filepath = os.path.join(out_dir, name + ".png"); sc.render.image_settings.file_format = "PNG"
    sc.render.image_settings.color_mode = "RGB"
    bpy.ops.render.render(write_still=True)

def env(k, v): return float(os.environ.get(k, v))
LK_H, LK_R, LK_T = env("LK_H", 0.42), env("LK_R", 0.42), env("LK_T", 0.42)     # light scale per shot, exposure stays at 0 under the Standard transform
WALL_K = env("WALL_K", 0.9)                                    # back-wall light: low enough that the black rear plate stops mirroring the backdrop
GRD_K = env("GRD_K", 1.1)
ONLY = os.environ.get("STRATA_ONLY")                          # render just one view, e.g. STRATA_ONLY=hero
hero_lights = [("key", (-0.9, -0.45, 0.85), 0.7, 22 * LK_H), ("fill", (0.9, -0.6, 0.5), 1.2, 3.5 * LK_H),
               ("camleft", (-0.94, -0.03, 0.34), 0.9, 7 * LK_H),                    # weak fill from camera-left at 20 deg elevation, both trunk walls catch a line
               ("top", (0.0, 0.0, 1.2), 0.35, 3.0 * LK_H),                          # small overhead: a tight grounding shadow under every edge
               ("wall", (-0.3, -0.9, 0.8), 2.0, 60 * GRD_K, (0, 2.9, 2.6))]
rear_lights = [("key", (0.6, 0.8, 0.45), 0.8, 20 * LK_R), ("fill", (-0.8, 0.5, 0.4), 1.2, 4 * LK_R), ("top", (0, 0, 1.2), 1.2, 2 * LK_R), ("wall", (0.3, 0.9, 0.8), 2.0, 60 * WALL_K, (0, -2.9, 2.6))]
top_lights = [("rake", (-0.9, -0.3, 0.22), 0.4, 26 * LK_T), ("top", (0, 0, 1.0), 1.0, 1.2 * LK_T)]
FACE_Y = -100.0
hero_pts = {"ring0": (0, FACE_Y, 3.25), "ring5": (0, FACE_Y, 48.4), "cap": (0, FACE_Y, 53.5), "ground": (140, -160, 0)}
rear_pts = {"strip_bottom": (0, 100.0, 5.7), "strip_top": (0, 100.0, 52.3), "ground": (-140, 160, 0)}
if ONLY in (None, "hero"): shoot("hero", -22, 32, 1.2, 85, (72, 0, 8), hero_lights, 0.0, pts=hero_pts)
if ONLY in (None, "rear"): shoot("rear", 196, 14, 1.2, 85, (72, 0, 22), rear_lights, 0.0, samples=SAMPLES_REAR, pts=rear_pts)
if ONLY in (None, "top"): shoot("top", 0, 89, 0.253, 85, (0, 0, 55), top_lights, 0.0)
import json as _j; _j.dump(PROJ, open(os.path.join(out_dir, "proj.json"), "w"))
