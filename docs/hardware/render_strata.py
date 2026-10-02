# Strata Kit product renders, from the real CAD. One script, three views.
#   python3 docs/hardware/render_strata.py [outdir]     (default docs/hardware/render)
# Step 1 re-runs strata_cad.py (all assertions, STLs, STEP, manifest) and has it write assembled-position meshes.
# Step 2 runs Blender headless on those meshes: cream ground, soft light, hero / rear / top views, 1600 px wide PNG.
# Each ring is its own flat PLA colour, terracotta at the base to cream at the cap. No metal, no gloss, no text.
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
    subprocess.run(["uv", "run", "--with", "build123d", "python", os.path.join(HERE, "strata_cad.py"), HERE], check=True, env=env)
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
sc.cycles.samples = 8 if PREVIEW else 40
sc.cycles.use_denoising = True
sc.cycles.max_bounces = 6
sc.render.resolution_x, sc.render.resolution_y, sc.render.resolution_percentage = W, H, 100
sc.view_settings.view_transform = "Standard"
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

# the machine: ring i in its own colour, cap cream, tray and rear plate black, rods plain steel grey (hidden inside the rings)
for i in range(6): load(f"ring{i}", material(f"ring{i}", mix(BASE, TOPC, i / 6)))
load("cap", material("cap", TOPC))
load("tray", material("tray", INK, 0.6)); load("rear", material("rear", INK, 0.6))
for i in range(4): load(f"rod{i}", material("rod", (120, 120, 120), 0.6))

# reference block, 127 x 127 x 50 mm, plain matte grey, uniform 4 mm radius, no marks
bpy.ops.mesh.primitive_cube_add(size=1)
blk = bpy.context.object; blk.name = "block"
blk.scale = (BLOCK[0] * S, BLOCK[1] * S, BLOCK[2] * S); blk.location = (205 * S, -25 * S, BLOCK[2] / 2 * S)
bpy.ops.object.transform_apply(scale=True)
bv = blk.modifiers.new("bevel", "BEVEL"); bv.width = 4 * S; bv.segments = 6; bv.limit_method = "NONE"
blk.data.materials.append(material("block", (176, 172, 165), 0.5))
for poly in blk.data.polygons: poly.use_smooth = True

# cream ground
bpy.ops.mesh.primitive_plane_add(size=40, location=(0, 0, 0))
gnd = bpy.context.object; gnd.data.materials.append(material("ground", CREAM, 0.85))
sc.world = bpy.data.worlds.new("w"); sc.world.use_nodes = True
bg = next(n for n in sc.world.node_tree.nodes if n.type == "BACKGROUND")
bg.inputs["Color"].default_value = lin(CREAM); bg.inputs["Strength"].default_value = 0.30

def area(name, loc, size, power, target=(0, 0, 0.02)):
    d = bpy.data.lights.new(name, "AREA"); d.size = size; d.energy = power; d.color = (1.0, 0.97, 0.92)
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
key_front = [("key", (-0.55, -0.7, 0.8), 0.9, 9), ("fill", (0.9, -0.3, 0.4), 0.9, 2.5), ("top", (0.0, 0.0, 1.2), 1.2, 2.5)]
ONLY = os.environ.get("STRATA_ONLY")                          # render just one view, e.g. STRATA_ONLY=hero
if ONLY in (None, "hero"): shoot("hero", -32, 22, 1.15, 85, (62, 0, 18), key_front, 0.5)
if ONLY in (None, "rear"): shoot("rear", 200, 14, 0.78, 85, (0, 0, 24), [("key", (0.5, 0.8, 0.7), 0.9, 9), ("fill", (-0.8, 0.3, 0.4), 0.9, 2.5), ("top", (0, 0, 1.2), 1.2, 2.5)], 0.5)
if ONLY in (None, "top"): shoot("top", 0, 88, 0.36, 85, (0, 0, 55), [("rake", (-0.5, -0.35, 0.28), 0.5, 5), ("top", (0, 0, 1.0), 1.0, 2)], 0.1)
