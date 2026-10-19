# SPDX-License-Identifier: CC-BY-NC-SA-4.0
# Copyright (c) 2026 Joshua Trommel. Hardware design, see docs/hardware/LICENSE-NOTICE.md
# Nimbus product renders from the real CAD: nimbus-hero.jpg (3/4 front) and nimbus-rear.jpg, 1600 px wide.
#   python3 docs/hardware/render_nimbus.py [outdir]     (default docs/hardware)
# Step 1 runs nimbus_cad.py (all its asserts) and has it write assembled-position meshes to a temp folder. Step 2 runs Blender
# headless (never a window) in Cycles: one soft area light overhead, a curved off-white sweep, one cream material, the desktop
# screenshot on the display. Step 3 flattens the PNGs to JPEGs with magick.
# NIMBUS_PREVIEW=1 renders 640 px at low samples for checking.
import sys, os, subprocess, tempfile, shutil, math, json

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from render_kit import build, blender, argv, lin
PREVIEW = bool(os.environ.get("NIMBUS_PREVIEW"))
W = int(os.environ.get("NIMBUS_W", 640 if PREVIEW else 1600)); H = int(os.environ.get("NIMBUS_H", 0)) or round(W * 0.6875)
SS = int(os.environ.get("SS", 1 if PREVIEW else 2))      # render at SS times the size, then Lanczos down: tack sharp edges and screen text
CREAM, INK, TERRA = (0xEF, 0xEB, 0xE4), (0x1C, 0x1C, 0x1E), (0xB5, 0x50, 0x2C)
SWEEP = (0xC9, 0xC4, 0xBB)
DESKTOP = os.path.join(HERE, "nimbus", "nimbus-desktop.png")
VIEWS = [("hero", (-620.0, -930.0, 560.0), (-30.0, 20.0, 100.0), 85.0),
         ("side", (15.0, -1250.0, 330.0), (15.0, 0.0, 100.0), 85.0),
         ("rear", (-620.0, -930.0, 560.0), (30.0, -20.0, 100.0), 85.0)]      # the rear is the hero's camera on the product turned half a turn, so backdrop, light and exposure are the same
BOX = bool(os.environ.get("NIMBUS_BOX"))      # NIMBUS_BOX=1: render the retail box (nimbus-box.jpg) in the same studio instead of the product
if BOX: VIEWS = [("box", (-570.0, -1200.0, 1320.0), (0.0, 0.0, 70.0), 85.0)]
# Wallpapers: NIMBUS_WALL=NAME with NIMBUS_W/NIMBUS_H and CAM="x,y,z", AIM="x,y,z", LENS=mm renders nimbus-NAME.jpg, the hero lit the same way
if os.environ.get("NIMBUS_WALL"):
    _v = lambda k: tuple(float(c) for c in os.environ[k].split(","))
    VIEWS = [(os.environ["NIMBUS_WALL"], _v("CAM"), _v("AIM"), float(os.environ.get("LENS", 85)))]

try:
    import bpy
except ImportError:
    bpy = None

if bpy is None:
    out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else HERE)
    tmp = tempfile.mkdtemp(prefix="nimbus-")
    try:
        env = dict(os.environ, NIMBUS_MESH=os.path.join(tmp, "mesh"))
        if BOX:       # no CAD: a stub mesh list, the lid print, a throwaway screen
            import nimbus_box
            os.makedirs(os.path.join(tmp, "mesh")); nimbus_box.label(os.path.join(tmp, "label.png"))
            json.dump({"meshes": [], "screen": [[0, 0, 0]] * 4}, open(os.path.join(tmp, "mesh", "meshes.json"), "w"))
        else:
            build("nimbus_cad.py", tmp, env=env, extra=("--with", "shapely"))
        shutil.copy(DESKTOP, os.path.join(tmp, "screen.png"))      # nimbus_desktop.py already composed the native apps on the real desktop
        blender(__file__, os.path.join(tmp, "mesh"), tmp)
        for name, *_ in VIEWS:
            if not os.path.exists(os.path.join(tmp, name + ".png")): continue
            subprocess.run(["magick", os.path.join(tmp, name + ".png"), "-filter", "Lanczos", "-resize", "%dx" % W, "-strip", "-quality", "90", os.path.join(out, "nimbus-%s.jpg" % name)], check=True)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    sys.exit(0)

import bmesh
from mathutils import Vector
mesh_dir, tmp = argv()[:2]
S = 0.001
bpy.ops.wm.read_factory_settings(use_empty=True)
sc = bpy.context.scene
sc.render.engine = "CYCLES"
sc.cycles.samples = int(os.environ.get("SAMPLES", 24 if PREVIEW else 400))
sc.cycles.use_denoising = True
sc.cycles.max_bounces = 8
sc.render.resolution_x, sc.render.resolution_y, sc.render.resolution_percentage = W * SS, H * SS, 100
sc.render.filter_size = 0.8
sc.view_settings.view_transform = "Standard"
try: sc.view_settings.look = "None"
except TypeError: pass
sc.view_settings.exposure = float(os.environ.get("EXPOSURE", 0.0))
try:
    prefs = bpy.context.preferences.addons["cycles"].preferences
    prefs.compute_device_type = "METAL"; prefs.get_devices()
    for d in prefs.devices: d.use = True
    sc.cycles.device = "GPU"
except Exception:
    sc.cycles.device = "CPU"

def socket(b, *names):
    for n in names:
        if n in b.inputs: return b.inputs[n]
    raise KeyError(names)
def mat(name, rgb, rough, sss=0.0, spec=0.35):
    m = bpy.data.materials.new(name); m.use_nodes = True
    b = next(n for n in m.node_tree.nodes if n.type == "BSDF_PRINCIPLED")
    b.inputs["Base Color"].default_value = lin(rgb); b.inputs["Roughness"].default_value = rough
    socket(b, "Specular IOR Level", "Specular").default_value = spec
    if sss:
        socket(b, "Subsurface Weight", "Subsurface").default_value = sss
        socket(b, "Subsurface Radius").default_value = (3.0, 2.4, 1.8)
        try: b.inputs["Subsurface Scale"].default_value = 0.004
        except KeyError: pass
    return m

meta = json.load(open(os.path.join(mesh_dir, "meshes.json")))
def glass_sheen(m):
    """one faint soft reflection of the sky, strongest along the top edge of the glass (Generated coordinates run 0 to 1 over the sheet)"""
    nt = m.node_tree; b = next(n for n in nt.nodes if n.type == "BSDF_PRINCIPLED")
    tc = nt.nodes.new("ShaderNodeTexCoord"); sp = nt.nodes.new("ShaderNodeSeparateXYZ"); mr = nt.nodes.new("ShaderNodeMapRange")
    nt.links.new(tc.outputs["Generated"], sp.inputs[0]); nt.links.new(sp.outputs["Z"], mr.inputs["Value"])
    mr.inputs["From Min"].default_value = 0.35; mr.inputs["From Max"].default_value = 1.0; mr.inputs["To Min"].default_value = 0.0; mr.inputs["To Max"].default_value = float(os.environ.get("SHEEN", 0.045))
    mr.clamp = True
    b.inputs["Emission Color"].default_value = (0.82, 0.86, 0.95, 1.0); nt.links.new(mr.outputs["Result"], b.inputs["Emission Strength"])
M = {"cream": mat("cream", tuple(min(c, 238) for c in CREAM), 0.62, float(os.environ.get("SSS", 0.1)), 0.15), "ink": mat("ink", INK, 0.3, 0.0, 0.5),
     "glass": mat("glass", INK, 0.06, 0.0, 0.25), "terra": mat("terra", TERRA, 0.6)}
glass_sheen(M["glass"])
USE = {"front_shell": "cream", "back_cover": "cream", "ground_inlay": "terra", "slot_inlay": "ink", "glass": "glass", "module": "ink", "sdliner": "ink", "driver": "ink", "ports": "ink", "board": "ink"}
for name in meta["meshes"]:
    if name in os.environ.get("SKIP", "").split(","): continue
    p = os.path.join(mesh_dir, name + ".stl")
    try: bpy.ops.wm.stl_import(filepath=p)
    except AttributeError: bpy.ops.import_mesh.stl(filepath=p)
    o = bpy.context.selected_objects[0]; o.name = name; o.scale = (S, S, S)
    bpy.context.view_layer.objects.active = o; bpy.ops.object.transform_apply(scale=True)
    o.data.materials.append(M[USE[name]])
    for poly in o.data.polygons: poly.use_smooth = True
    mod = o.modifiers.new("edges", "EDGE_SPLIT"); mod.use_edge_angle = True; mod.split_angle = math.radians(14)
# the screen: the desktop screenshot on the active area, lit by itself
q = [Vector(c) * S for c in meta["screen"]]
me = bpy.data.meshes.new("screen"); me.from_pydata(q, [], [(0, 1, 2, 3)]); me.update()
uv = me.uv_layers.new(name="uv");
for i, c in enumerate(((0, 0), (1, 0), (1, 1), (0, 1))): uv.data[i].uv = c
scr = bpy.data.objects.new("screen", me); sc.collection.objects.link(scr)
sm = bpy.data.materials.new("screen"); sm.use_nodes = True; nt = sm.node_tree
for n in list(nt.nodes): nt.nodes.remove(n)
img = nt.nodes.new("ShaderNodeTexImage"); img.image = bpy.data.images.load(os.path.join(tmp, "screen.png"))
em = nt.nodes.new("ShaderNodeEmission"); em.inputs["Strength"].default_value = float(os.environ.get("SCREEN", 0.8))
outn = nt.nodes.new("ShaderNodeOutputMaterial")
# a soft diagonal sheen from the top left, so the screen reads as a lit display behind glass
tcn = nt.nodes.new("ShaderNodeTexCoord"); sep = nt.nodes.new("ShaderNodeSeparateXYZ"); inv = nt.nodes.new("ShaderNodeMath"); add = nt.nodes.new("ShaderNodeMath")
mrn = nt.nodes.new("ShaderNodeMapRange"); mix = nt.nodes.new("ShaderNodeMix"); mix.data_type = "RGBA"
nt.links.new(tcn.outputs["UV"], sep.inputs[0])
inv.operation = "SUBTRACT"; inv.inputs[0].default_value = 1.0; nt.links.new(sep.outputs["X"], inv.inputs[1])
add.operation = "ADD"; nt.links.new(inv.outputs[0], add.inputs[0]); nt.links.new(sep.outputs["Y"], add.inputs[1])
mrn.inputs["From Min"].default_value = 1.0; mrn.inputs["From Max"].default_value = 1.8; mrn.inputs["To Min"].default_value = 0.0; mrn.inputs["To Max"].default_value = float(os.environ.get("GLARE", 0.2)); mrn.clamp = True
nt.links.new(add.outputs[0], mrn.inputs["Value"])
nt.links.new(mrn.outputs["Result"], mix.inputs["Factor"]); nt.links.new(img.outputs["Color"], mix.inputs["A"]); mix.inputs["B"].default_value = (1.0, 1.0, 1.0, 1.0)
nt.links.new(mix.outputs["Result"], em.inputs["Color"]); nt.links.new(em.outputs["Emission"], outn.inputs["Surface"])
me.materials.append(sm)
if BOX:
    bpy.data.objects.remove(scr)
    import nimbus_box
    PROD = nimbus_box.build(bpy, M, os.path.join(tmp, "label.png"))
else:
    PROD = [o for o in bpy.data.objects]    # everything so far is the product; the side view turns it a quarter turn so the backdrop matches the hero
# the sweep: a floor that curves up behind into a wall, off-white, seamless
import math as _m
prof = [(0.0, 0.0), (1.4, 0.0)] + [(1.4 + 1.2 * _m.sin(_m.radians(a)), 1.2 - 1.2 * _m.cos(_m.radians(a))) for a in range(10, 91, 10)] + [(2.6, 5.0)]
bm = bmesh.new(); vs = [bm.verts.new((r, 0.0, z)) for r, z in prof]
for i in range(len(vs) - 1): bm.edges.new((vs[i], vs[i + 1]))
bmesh.ops.spin(bm, geom=bm.verts[:] + bm.edges[:], cent=(0, 0, 0), axis=(0, 0, 1), angle=_m.tau, steps=64)
bmesh.ops.remove_doubles(bm, verts=bm.verts[:], dist=1e-5)
me2 = bpy.data.meshes.new("sweep"); bm.to_mesh(me2); bm.free()
sw = bpy.data.objects.new("sweep", me2); sc.collection.objects.link(sw)
for pl in me2.polygons: pl.use_smooth = True
sweep_m = mat("sweep", SWEEP, 0.5, 0.0, 0.12)
_b = next(n for n in sweep_m.node_tree.nodes if n.type == "BSDF_PRINCIPLED"); _b.inputs["Emission Color"].default_value = lin(SWEEP); _b.inputs["Emission Strength"].default_value = float(os.environ.get("LIFT", 0.04))
sw.data.materials.append(sweep_m)
# light: one big soft area above, slightly forward; a dim world for the falloff down the face
def area(name, size, power, loc, target):
    d = bpy.data.lights.new(name, "AREA"); d.shape = "RECTANGLE"; d.size, d.size_y = size; d.energy = power
    d.color = (1.0, 0.93, 0.84)      # a warm key, like tungsten through a softbox, so the cream reads cream where the light hits it square (the rear)
    o = bpy.data.objects.new(name, d); sc.collection.objects.link(o)
    o.location = loc; o.rotation_euler = (Vector(target) - Vector(loc)).to_track_quat("-Z", "Y").to_euler(); return o
key = area("key", (1.6, 1.2), float(os.environ.get("KEY", 70)), (0.0, 0.1, 1.5), (0.0, -0.05, 0.1))
wd = bpy.data.worlds.new("w"); wd.use_nodes = True; sc.world = wd
bg = next(n for n in wd.node_tree.nodes if n.type == "BACKGROUND"); bg.inputs["Color"].default_value = lin(SWEEP); bg.inputs["Strength"].default_value = float(os.environ.get("WORLD", 0.2))
cd = bpy.data.cameras.new("c"); cd.clip_end = 20.0; cam = bpy.data.objects.new("c", cd); sc.collection.objects.link(cam); sc.camera = cam
for name, loc, target, lens in VIEWS:
    if os.environ.get("VIEW") and name not in os.environ["VIEW"].split(","): continue
    cd.lens = lens
    for o in PROD: o.rotation_euler[2] = {"side": -math.pi / 2, "rear": math.pi}.get(name, 0.0)
    sc.view_settings.exposure = float(os.environ.get("EXPOSURE", -0.35))      # one exposure and one overhead light for every view
    l = Vector(loc) * S; t = Vector(target) * S
    cam.location = l; cam.rotation_euler = (t - l).to_track_quat("-Z", "Y").to_euler()
    sc.render.filepath = os.path.join(tmp, name + ".png"); bpy.ops.render.render(write_still=True)
