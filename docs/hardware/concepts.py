# SPDX-License-Identifier: CC-BY-NC-SA-4.0
# Copyright (c) 2026 Joshua Trommel. Hardware design, see docs/hardware/LICENSE-NOTICE.md
# Joshua Tree dev kit, original form concepts: "neo" (stacked sandstone strata,
# gaps are the vents) and "monolith" (standing desert stone). Headless EEVEE.
import bpy, math, sys, os
args = sys.argv[sys.argv.index("--") + 1:]
OUT, CONCEPT = args[0], args[1]
S = 0.01
bpy.ops.wm.read_factory_settings(use_empty=True)
scn = bpy.context.scene

def srgb(h):
    c = [int(h[i:i+2], 16) / 255 for i in (0, 2, 4)]
    return tuple(((x + 0.055) / 1.055) ** 2.4 if x > 0.04045 else x / 12.92 for x in c)
def mat(name, hexc, rough=0.5, metal=0.0):
    m = bpy.data.materials.new(name); m.use_nodes = True
    b = next(n for n in m.node_tree.nodes if n.type == "BSDF_PRINCIPLED")
    b.inputs["Base Color"].default_value = (*srgb(hexc), 1)
    b.inputs["Roughness"].default_value = rough; b.inputs["Metallic"].default_value = metal
    return m
CREAM = mat("cream", "EFE8DC", 0.6)
SAND = mat("sand", "E2D6C2", 0.7)
TERRA = mat("terra", "B9542C", 0.55)
INK = mat("ink", "1A1814", 0.55)
ENGRAVE = mat("engrave", "D8CBB6", 0.85)
CORE = mat("core", "2B2622", 0.8)
STEEL = mat("steel", "8E8A82", 0.35, 0.85)
TONGUE = mat("tongue", "E9E3D8", 0.5)
FLOOR = mat("floor", "D9CDB8", 0.95)
LED = mat("led", "E2733F", 0.3)
led_n = next(n for n in LED.node_tree.nodes if n.type == "BSDF_PRINCIPLED")
try:
    led_n.inputs["Emission Color"].default_value = (*srgb("FF7A3D"), 1); led_n.inputs["Emission Strength"].default_value = 6
except Exception: pass

def box(name, sx, sy, sz, loc, m, bevel=0.0, segs=6):
    bpy.ops.mesh.primitive_cube_add(size=1, location=loc)
    o = bpy.context.object; o.name = name; o.scale = (sx, sy, sz); bpy.ops.object.transform_apply(scale=True)
    if bevel:
        md = o.modifiers.new("bev", "BEVEL"); md.width = bevel; md.segments = segs; md.limit_method = "ANGLE"
    o.data.materials.append(m); return o
def cyl(r, d, loc, rot, m):
    bpy.ops.mesh.primitive_cylinder_add(radius=r, depth=d, location=loc, rotation=rot, vertices=48)
    bpy.context.object.data.materials.append(m); return bpy.context.object
def mark(width_mm, loc, rot=(0, 0, 0)):
    before = set(bpy.data.objects)
    bpy.ops.import_curve.svg(filepath=os.path.join(OUT, "mark.svg"))
    cs = [o for o in bpy.data.objects if o not in before and o.type == "CURVE"]
    bpy.ops.object.select_all(action="DESELECT")
    for c in cs: c.select_set(True)
    bpy.context.view_layer.objects.active = cs[0]
    if len(cs) > 1: bpy.ops.object.join()
    mk = bpy.context.object; mk.data.extrude = 0.00003
    bpy.ops.object.origin_set(type="ORIGIN_GEOMETRY", center="BOUNDS")
    k = (width_mm * S) / max(mk.dimensions.x, 1e-6); mk.scale = (k, k, 1)
    mk.location = loc; mk.rotation_euler = rot
    mk.data.materials.clear(); mk.data.materials.append(INK); return mk
def ports_row(y, z, x0, facing=1):
    # facing=+1: ports on the +Y face. Small, real shapes on a slim steel plate.
    box("shield", 150 * S, 1.2 * S, 20 * S, (x0, y, z), STEEL)
    dy = facing * 1.2 * S
    cyl(4.6 * S, 2.5 * S, (x0 - 62 * S, y + dy, z), (math.radians(90), 0, 0), CORE)
    for px in (-44, -28, 36, 52):
        box("usb", 13 * S, 2.5 * S, 5.6 * S, (x0 + px * S, y + dy, z), CORE); box("usbt", 11 * S, 2.8 * S, 1.8 * S, (x0 + px * S, y + dy, z + 0.8 * S), TONGUE)
    box("hdmi", 15 * S, 2.5 * S, 5.5 * S, (x0 - 9 * S, y + dy, z), CORE)
    box("eth", 16 * S, 2.5 * S, 13.5 * S, (x0 + 14 * S, y + dy, z), CORE); box("etht", 8 * S, 2.8 * S, 3 * S, (x0 + 14 * S, y + dy, z + 5.2 * S), TONGUE)
    cyl(3.2 * S, 2.5 * S, (x0 + 66 * S, y + dy, z), (math.radians(90), 0, 0), CORE)

if CONCEPT == "neo":
    # desert strata: warm at the base, cream at the cap, each layer a touch off-axis
    # like real sandstone. The dark core shows through the gaps, which are the vents.
    box("core", 178 * S, 178 * S, 62 * S, (0, 0, 32 * S), CORE, bevel=6 * S)
    tones = ["B9542C", "C9744A", "D8A27A", "E4C9A8", "ECDDC6", "F0E7D8"]
    thick = [9.0, 7.0, 10.0, 6.5, 8.5, 7.0]
    shift = [(0, 0), (1.6, -1.0), (-1.2, 0.8), (1.0, 1.4), (-1.5, -0.6), (0.6, -0.4)]
    twist = [0, 0.8, -0.6, 0.5, -0.9, 0.3]
    z, gap = 0.0, 2.0
    for i, t in enumerate(thick):
        w = 212 - i * 5.0
        o = box(f"stratum{i}", w * S, w * S, t * S, (shift[i][0] * S, shift[i][1] * S, (z + t / 2) * S),
                mat(f"t{i}", tones[i], 0.72 - i * 0.03), bevel=3.0 * S, segs=5)
        o.rotation_euler = (0, 0, math.radians(twist[i]))
        z += t + gap
    top_w = 212 - len(thick) * 5.0
    box("cap", top_w * S, top_w * S, 4.5 * S, (0, 0, (z + 2.25) * S), CREAM, bevel=2.2 * S, segs=5)
    H = (z + 4.5) * S
    cap = bpy.data.objects["cap"]
    mk = mark(80, (0, 6 * S, H))
    mk.data.extrude = 0.5 * S / mk.scale[0]
    bpy.ops.object.select_all(action="DESELECT"); mk.select_set(True); bpy.context.view_layer.objects.active = mk
    bpy.ops.object.convert(target="MESH")
    b = cap.modifiers.new("engrave", "BOOLEAN"); b.operation = "DIFFERENCE"; b.object = mk
    try: b.solver = "EXACT"
    except Exception: pass
    try: b.material_mode = "TRANSFER"
    except Exception: pass
    mk.data.materials.clear(); mk.data.materials.append(ENGRAVE); mk.hide_render = True
    cap.modifiers.new("tri", "TRIANGULATE")  # boolean ngons shade as a ghost rectangle without this
    # power: a flush ink disc on the cap; status: a glowing slit in the first gap
    cyl(4.5 * S, 0.8 * S, (top_w / 2 * 0.72 * S, -top_w / 2 * 0.72 * S, H), (0, 0, 0), INK)
    box("ledline", 30 * S, 1 * S, 1.0 * S, (0, -(178 / 2 + 0.4) * S, (thick[0] + gap / 2) * S), LED)
    # rear I/O notch: cut every stratum back to the core so the ports are reachable
    cutter = box("io_notch", 160 * S, 30 * S, 46 * S, (0, (178 / 2 + 15) * S, 30.5 * S), CORE)
    for i in range(len(thick)):
        o = bpy.data.objects[f"stratum{i}"]
        c = o.modifiers.new("io", "BOOLEAN"); c.operation = "DIFFERENCE"; c.object = cutter
    cutter.hide_render = True
    ports_row((178 / 2 + 0.6) * S, 30 * S, 0)
    look, hero, top_cam = (0, 0, 0.3), (-3.7, -4.4, 2.35), (0, -0.01, 5.4)
else:
    # monolith: a standing stone, tree engraved on the broad face, ports low on the back
    Wm, Dm, Hm = 170, 64, 230
    box("stone", Wm * S, Dm * S, Hm * S, (0, 0, (Hm / 2 + 14) * S), CREAM, bevel=18 * S, segs=10)
    box("plinth", (Wm + 24) * S, (Dm + 30) * S, 14 * S, (0, 0, 7 * S), INK, bevel=4 * S)
    box("band", (Wm + 1) * S, (Dm + 1) * S, 7 * S, (0, 0, 40 * S), TERRA, bevel=2 * S)
    mark(118, (0, -(Dm / 2 + 0.3) * S, (Hm * 0.62 + 14) * S), rot=(math.radians(90), 0, 0))
    box("ledline", 30 * S, 1 * S, 1.4 * S, (0, -(Dm / 2 + 0.6) * S, 22 * S), LED)
    cyl(5 * S, 2.4 * S, (60 * S, -(Dm / 2 + 0.8) * S, (Hm + 2) * S), (math.radians(90), 0, 0), INK)
    ports_row((Dm / 2 + 0.6) * S, 62 * S, 0)
    look, hero, top_cam = (0, 0, 1.25), (-3.6, -5.0, 2.3), (0, -6.0, 1.3)

bpy.ops.mesh.primitive_plane_add(size=400, location=(0, 0, -0.02)); bpy.context.object.data.materials.append(FLOOR)
bpy.ops.object.light_add(type="AREA", location=(-3, -4, 6)); k = bpy.context.object; k.data.energy = 520; k.data.size = 5; k.data.color = srgb("FFF1DE")
bpy.ops.object.light_add(type="AREA", location=(5, 1, 3)); f = bpy.context.object; f.data.energy = 160; f.data.size = 4; f.data.color = srgb("DDE6F2")
bpy.ops.object.light_add(type="AREA", location=(0, 6, 4)); r = bpy.context.object; r.data.energy = 220; r.data.size = 3
w = bpy.data.worlds.new("w"); scn.world = w; w.use_nodes = True
bg = next(n for n in w.node_tree.nodes if n.type == "BACKGROUND"); bg.inputs["Color"].default_value = (*srgb("EDE4D4"), 1); bg.inputs["Strength"].default_value = 0.55
for e in ("BLENDER_EEVEE_NEXT", "BLENDER_EEVEE", "CYCLES"):
    try: scn.render.engine = e; break
    except TypeError: continue
scn.render.resolution_x, scn.render.resolution_y = 1600, 1000
try:
    scn.view_settings.view_transform = "AgX"; scn.view_settings.exposure = 0.35
except Exception: pass
try: scn.eevee.use_shadows = True
except Exception: pass

def shot(name, loc, lk, lens=62):
    bpy.ops.object.camera_add(location=loc); cam = bpy.context.object; cam.data.lens = lens
    d = [lk[i] - loc[i] for i in range(3)]
    cam.rotation_euler = (math.atan2(math.hypot(d[0], d[1]), -d[2]), 0, math.atan2(d[1], d[0]) - math.pi / 2)
    scn.camera = cam; scn.render.filepath = os.path.join(OUT, name); bpy.ops.render.render(write_still=True)
shot(f"{CONCEPT}-hero.png", hero, look)
shot(f"{CONCEPT}-rear.png", (2.6, 4.6, 1.6 if CONCEPT == "neo" else 1.9), look)
print("RENDERED", CONCEPT)
