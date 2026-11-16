import bpy, math, sys
out = sys.argv[sys.argv.index("--") + 1]; TREE = sys.argv[sys.argv.index("--") + 2]
bpy.ops.wm.read_factory_settings(use_empty=True)
sc = bpy.context.scene
def mat(name, rgb, metal=0.0, rough=0.5):
    m = bpy.data.materials.new(name); m.use_nodes = True
    b = m.node_tree.nodes["Principled BSDF"]; b.inputs["Base Color"].default_value = (*rgb, 1)
    b.inputs["Metallic"].default_value = metal; b.inputs["Roughness"].default_value = rough; return m
PCB = mat("pcb", (0.035, 0.025, 0.02), rough=0.35)          # bark-ink solder mask
GOLD = mat("gold", (0.85, 0.62, 0.25), 1.0, 0.25)
STEEL = mat("steel", (0.75, 0.75, 0.78), 1.0, 0.3)
BLACK = mat("black", (0.02, 0.02, 0.02), rough=0.6)
CREAM = mat("cream", (0.92, 0.9, 0.84), rough=0.5)
RUST = mat("rust", (0.71, 0.31, 0.17), rough=0.45)          # the terracotta accent
GREEN = mat("cmgreen", (0.05, 0.25, 0.12), rough=0.4)
def box(x, y, z, w, d, h, m, bevel=0.0):
    bpy.ops.mesh.primitive_cube_add(location=(x + w / 2, y + d / 2, z + h / 2)); o = bpy.context.object
    o.scale = (w / 2, d / 2, h / 2); o.data.materials.append(m)
    if bevel: mod = o.modifiers.new("b", "BEVEL"); mod.width = bevel; mod.segments = 3
    return o
def cyl(x, y, z, r, h, m):
    bpy.ops.mesh.primitive_cylinder_add(radius=r, depth=h, location=(x, y, z + h / 2)); bpy.context.object.data.materials.append(m)
# the board, 160 x 90 x 1.6 mm (units: cm, so 16 x 9)
box(0, 0, 0, 16, 9, 0.16, PCB, 0.3)
for hx, hy in ((0.4, 0.4), (15.6, 0.4), (0.4, 8.6), (15.6, 8.6)): cyl(hx, hy, 0.16, 0.28, 0.01, GOLD)
# the CM5 module on its connectors
box(5.2, 2.6, 0.16, 5.5, 4.0, 0.25, BLACK)
box(5.2, 2.6, 0.41, 5.5, 4.0, 0.12, GREEN, 0.05)
box(6.4, 3.6, 0.53, 1.6, 1.6, 0.12, STEEL, 0.03)          # the SoC's lid
for i in range(4): box(8.5 + i * 0.5, 3.4, 0.53, 0.4, 0.9, 0.1, BLACK)   # RAM and eMMC
# back edge ports
box(-0.15, 0.8, 0.16, 0.9, 0.9, 0.35, STEEL, 0.05)                         # USB-C power
for x in (1.2, 3.0): box(x, -0.15, 0.16, 1.5, 1.1, 0.6, STEEL, 0.03)        # HDMI
box(4.8, -0.25, 0.16, 1.6, 2.1, 1.35, STEEL, 0.03)                         # Ethernet
for x in (6.8, 8.6): box(x, -0.25, 0.16, 1.5, 1.7, 1.5, STEEL, 0.03)        # USB-A stacked
box(10.4, -0.15, 0.16, 0.9, 0.8, 0.35, STEEL, 0.05)                        # USB-C data
cyl(12.0, 0.4, 0.16, 0.32, 0.6, BLACK)                                     # audio jack
# top edge
box(0.8, 7.9, 0.16, 5.1, 0.5, 0.85, BLACK)                                  # GPIO header
for i in range(20):
    for j in range(2): box(0.93 + i * 0.254, 8.0 + j * 0.254, 1.0, 0.06, 0.06, 0.6, GOLD)
box(6.4, 7.9, 0.16, 0.8, 0.45, 0.55, CREAM)                                  # fan header
cyl(8.3, 8.2, 0.16, 0.5, 0.3, STEEL)                                         # RTC cell
box(14.0, 7.8, 0.16, 1.2, 0.6, 0.35, BLACK); box(14.3, 7.9, 0.5, 0.6, 0.4, 0.2, RUST)   # power button
# NVMe slot and drive
box(11.2, 2.4, 0.16, 0.6, 2.2, 0.45, BLACK)
box(11.8, 2.6, 0.32, 4.0, 1.8, 0.12, PCB); box(12.6, 2.9, 0.44, 2.4, 1.2, 0.1, BLACK)
# small parts, a PMIC and an inductor
box(1.0, 4.2, 0.16, 1.2, 1.2, 0.15, BLACK); cyl(3.0, 4.8, 0.16, 0.5, 0.5, BLACK)
import random; r = random.Random(4)
for _ in range(60):   # caps and resistors
    x, y = r.uniform(0.8, 15), r.uniform(1.6, 7.4)
    if 5.0 < x < 11.0 and 2.4 < y < 6.8: continue
    box(x, y, 0.16, 0.2, 0.12, 0.08, CREAM if r.random() < 0.6 else BLACK)
# the mark in terracotta silkscreen: a small raised plate as a stand-in
box(11.9, 5.1, 0.16, 4.2, 0.03, 0.004, RUST)

def label(txt, x, y, size=0.22, m=None, rot=0.0):   # silkscreen: flat cream text on the board
    bpy.ops.object.text_add(location=(x, y, 0.161)); t = bpy.context.object
    t.data.body = txt; t.data.size = size; t.rotation_euler = (0, 0, rot); t.data.materials.append(m or CREAM)
def hole(x, y, z, w, d, h, m):   # a dark opening on a port's face (the face toward -y)
    box(x, y - 0.01, z, w, 0.04, h, m)
BLUE = mat("blue", (0.1, 0.25, 0.7), rough=0.4)
# openings and tongues on the ports
for x in (6.8, 8.6):
    for k in (0, 1): hole(x + 0.2, -0.27, 0.3 + k * 0.7, 1.1, 0.04, 0.45, BLACK); box(x + 0.35, -0.24, 0.42 + k * 0.7, 0.8, 0.5, 0.12, BLUE)
hole(5.05, -0.27, 0.35, 1.1, 0.04, 0.8, BLACK)
cyl(5.2, -0.27, 1.25, 0.06, 0.01, GOLD); 
for x in (1.2, 3.0): hole(x + 0.2, -0.17, 0.3, 1.1, 0.04, 0.3, BLACK)
hole(0.0, 0.8, 0.22, 0.04, 0.6, 0.18, BLACK)
# gold pads and traces fanning out from the module
r2 = random.Random(9)
for i in range(26):
    y = 2.9 + i * 0.13
    box(4.2 - r2.uniform(0, 2.5), y, 0.16, r2.uniform(0.8, 2.6), 0.03, 0.004, GOLD)
for i in range(18):
    x = 5.6 + i * 0.27
    box(x, 1.2, 0.16, 0.03, 1.35, 0.004, GOLD)
# electrolytic capacitors with cream tops, an inductor
for cx, cy in ((13.0, 6.4), (14.2, 6.4), (2.5, 6.6)):
    cyl(cx, cy, 0.16, 0.42, 0.85, STEEL); cyl(cx, cy, 1.01, 0.42, 0.02, CREAM)
box(2.4, 4.1, 0.16, 1.0, 1.0, 0.45, BLACK, 0.12)
# labels
label("JOSHUA TREE  board 1", 11.9, 5.25, 0.3)
for txt, x, y in (("HDMI0", 1.4, 1.05), ("HDMI1", 3.2, 1.05), ("ETH", 5.3, 2.0), ("USB3", 7.1, 1.6), ("USB3", 8.9, 1.6),
                  ("PWR", 0.95, 1.85), ("GPIO", 0.9, 7.45), ("FAN", 6.4, 7.5), ("RTC", 7.95, 7.45), ("NVMe", 11.3, 4.75), ("CM5", 5.3, 6.75)):
    label(txt, x, y, 0.2)
# the mark: tree.png on a plane, alpha kept
import os
img = bpy.data.images.load(TREE)
mm = bpy.data.materials.new("mark"); mm.use_nodes = True; nt = mm.node_tree
tex = nt.nodes.new("ShaderNodeTexImage"); tex.image = img; b = nt.nodes["Principled BSDF"]
nt.links.new(tex.outputs["Color"], b.inputs["Base Color"]); nt.links.new(tex.outputs["Alpha"], b.inputs["Alpha"])
mm.blend_method = "BLEND" if hasattr(mm, "blend_method") else None
bpy.ops.mesh.primitive_plane_add(size=1.6, location=(14.6, 4.9, 0.163)); bpy.context.object.data.materials.append(mm)

# floor, lights, camera
bpy.ops.mesh.primitive_plane_add(size=80, location=(8, 4.5, -0.01)); bpy.context.object.data.materials.append(mat("floor", (0.55, 0.52, 0.62), rough=0.9))
bpy.ops.object.light_add(type="AREA", location=(4, -6, 14)); L = bpy.context.object; L.data.energy = 4000; L.data.size = 12
bpy.ops.object.light_add(type="SUN", location=(0, 0, 10)); bpy.context.object.data.energy = 2.5; bpy.context.object.rotation_euler = (0.6, 0.2, 0.8)
bpy.ops.object.camera_add(location=(-5.5, -12.5, 15)); cam = bpy.context.object; sc.camera = cam
c = cam.constraints.new("TRACK_TO"); bpy.ops.object.empty_add(location=(8, 4.2, 0)); c.target = bpy.context.object
cam.data.lens = 42
sc.render.engine = "BLENDER_EEVEE_NEXT" if "BLENDER_EEVEE_NEXT" in [e.identifier for e in bpy.types.RenderSettings.bl_rna.properties["engine"].enum_items] else "BLENDER_EEVEE"
sc.render.resolution_x, sc.render.resolution_y = 2000, 1250
sc.world = bpy.data.worlds.new("w"); sc.world.use_nodes = True
sc.world.node_tree.nodes["Background"].inputs[0].default_value = (0.6, 0.58, 0.7, 1); sc.world.node_tree.nodes["Background"].inputs[1].default_value = 0.6
sc.render.filepath = out
bpy.ops.render.render(write_still=True)
