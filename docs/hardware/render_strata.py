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

if bpy is None:
    out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "render"))
    mesh = os.path.join(out, "mesh"); os.makedirs(mesh, exist_ok=True)
    env = dict(os.environ, STRATA_MESH=mesh)
    subprocess.run(["uv", "run", "--with", "build123d", "--with", "shapely", "python", os.path.join(HERE, "strata_cad.py"), HERE], check=True, env=env)
    subprocess.run(["/opt/homebrew/bin/blender", "-b", "-P", __file__, "--", mesh, out], check=True)
    sys.exit(0)

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
sc.cycles.use_denoising = True
sc.cycles.max_bounces = 6
sc.render.resolution_x, sc.render.resolution_y, sc.render.resolution_percentage = W, H, 100
sc.view_settings.view_transform = "AgX"
try: sc.view_settings.look = "AgX - Medium High Contrast"
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
    h = m["hex"].lstrip("#"); rgb = tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))
    load(m["name"], material(m["name"], rgb, 0.6))

# stand-in stock I/O shield (158.75 x 44.45, steel) seated at the board's rear edge, y = 88, with the real J4125B-ITX ports cut into it.
# Left to right seen from behind: PS/2, 2x USB 2.0 stack, VGA, DVI-D, HDMI, 2x USB 3.2 stack, RJ45, three audio jacks. mm: (x0, x1, z0, z1), ports sit on the PCB at z 8.6.
IO_Z = 29.0
bpy.ops.mesh.primitive_cube_add(size=1)
shd = bpy.context.object; shd.name = "shield"; shd.scale = (158.75 * S, 0.8 * S, 44.45 * S); shd.location = (0, 88.4 * S, IO_Z * S)
bpy.ops.object.transform_apply(scale=True); shd.data.materials.append(material("shield", (158, 156, 150), 0.45))
PZ = 9.0
PORTS = [(-76.8, -65.8, PZ, PZ + 11),                                   # PS/2 mini-DIN
         (-62.3, -49.3, PZ, PZ + 7), (-62.3, -49.3, PZ + 8.5, PZ + 15.5),  # 2x USB 2.0
         (-45.8, -17.8, PZ, PZ + 12.5),                                 # VGA
         (-14.3, 13.7, PZ, PZ + 12.5),                                  # DVI-D
         (17.2, 31.2, PZ, PZ + 6),                                      # HDMI
         (34.7, 47.7, PZ, PZ + 7), (34.7, 47.7, PZ + 8.5, PZ + 15.5),  # 2x USB 3.2
         (51.2, 66.2, PZ, PZ + 13.5),                                   # RJ45
         (69.7, 76.7, PZ, PZ + 7), (69.7, 76.7, PZ + 8.2, PZ + 15.2), (69.7, 76.7, PZ + 16.4, PZ + 23.4)]   # 3 audio jacks
for (x0, x1, z0, z1) in PORTS:
    bpy.ops.mesh.primitive_cube_add(size=1); p = bpy.context.object
    p.scale = ((x1 - x0) * S, 0.4 * S, (z1 - z0) * S); p.location = ((x0 + x1) / 2 * S, 88.9 * S, (z0 + z1) / 2 * S)
    bpy.ops.object.transform_apply(scale=True); p.data.materials.append(material("port", INK, 0.7))

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
bg.inputs["Color"].default_value = lin(CREAM); bg.inputs["Strength"].default_value = 0.35

def area(name, loc, size, power, target=(0, 0, 0.02)):
    d = bpy.data.lights.new(name, "AREA"); d.size = size; d.energy = power; d.color = (1.0, 0.97, 0.92)
    if name.startswith("wall"): d.specular_factor = 0.0                 # the backdrop lights only light the sweep, they never mirror in the product
    o = bpy.data.objects.new(name, d); sc.collection.objects.link(o); o.location = loc
    o.rotation_euler = (Vector(target) - Vector(loc)).to_track_quat("-Z", "Y").to_euler()
    return o

cam_d = bpy.data.cameras.new("cam"); cam = bpy.data.objects.new("cam", cam_d); sc.collection.objects.link(cam); sc.camera = cam
cam_d.sensor_width = 36; cam_d.clip_start = 0.01; cam_d.clip_end = 50

def shoot(name, az, el, dist, lens, target, lights, ev=0.0):
    sc.view_settings.exposure = ev
    for o in [o for o in sc.objects if o.type == "LIGHT"]: bpy.data.objects.remove(o)
    for l in lights: area(*l)
    a, e = math.radians(az), math.radians(el)
    t = Vector([v * S for v in target])
    cam.location = t + Vector((math.sin(a) * math.cos(e), -math.cos(a) * math.cos(e), math.sin(e))) * dist
    cam.rotation_euler = (t - cam.location).to_track_quat("-Z", "Y").to_euler()
    cam_d.lens = lens
    sc.render.filepath = os.path.join(out_dir, name + ".png"); sc.render.image_settings.file_format = "PNG"
    sc.render.image_settings.color_mode = "RGB"
    bpy.ops.render.render(write_still=True)

soft = lambda x, y, z: (x, y, z)
key_front = [("key", (-0.9, -0.45, 0.42), 0.7, 22), ("fill", (0.9, -0.6, 0.5), 1.2, 3.5), ("top", (0.0, 0.0, 1.2), 1.2, 2.0)]
EV_H, EV_R, EV_T = [float(os.environ.get(k, v)) for k, v in (('EV_H', 1.3), ('EV_R', -3.0), ('EV_T', 1.45))]                         # exposure per view. The rear sweep is lit hard from the front-left so the back wall stays cream, so its exposure is pulled down to keep the floor and the cap under 250
WALL_K = float(os.environ.get('WALL_K', 7.5))                # rear shot: how hard the back wall of the sweep is lit
KEY_K = float(os.environ.get('KEY_K', 0.85))                # rear shot: scale on the front-side lights
ONLY = os.environ.get("STRATA_ONLY")                          # render just one view, e.g. STRATA_ONLY=hero
if ONLY in (None, "hero"): shoot("hero", -22, 32, 1.2, 85, (72, 0, 8), key_front, EV_H)
if ONLY in (None, "rear"): shoot("rear", 196, 14, 1.2, 85, (72, 0, 22), [("key", (0.6, 0.8, 0.45), 0.8, 20 * KEY_K), ("fill", (-0.8, 0.5, 0.4), 1.2, 4 * KEY_K), ("top", (0, 0, 1.2), 1.2, 2 * KEY_K), ("wall", (0.3, 0.9, 0.8), 2.0, 200 * WALL_K, (0, -2.9, 2.6))], EV_R)
if ONLY in (None, "top"): shoot("top", 0, 89, 0.253, 85, (0, 0, 55), [("rake", (-0.9, -0.3, 0.22), 0.4, 26), ("top", (0, 0, 1.0), 1.0, 1.2)], EV_T)
