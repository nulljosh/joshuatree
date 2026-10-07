# Draws docs/hardware/breadboard.png: the Joshua Tree breadboard prototype (a Pi 4, a 40-pin breakout, LED, button,
# speaker, serial header). Run: blender -b --factory-startup -P tools/gen/breadboard.py -- docs/hardware/breadboard.png
# The wiring it shows is written out in docs/hardware/BREADBOARD.md; change both together.
import bpy, math, sys
from mathutils import Vector
OUT = sys.argv[sys.argv.index('--') + 1]
bpy.ops.wm.read_factory_settings(use_empty=True)
sc = bpy.context.scene
P = 0.254  # one hole pitch, in cm (1 unit = 1 cm)

def mat(name, rgb, rough=0.5, metal=0.0, emit=0.0):
    m = bpy.data.materials.new(name); m.use_nodes = True
    b = next(n for n in m.node_tree.nodes if n.type == 'BSDF_PRINCIPLED')
    b.inputs['Base Color'].default_value = (*rgb, 1); b.inputs['Roughness'].default_value = rough; b.inputs['Metallic'].default_value = metal
    if emit:
        b.inputs['Emission Color'].default_value = (*rgb, 1); b.inputs['Emission Strength'].default_value = emit
    return m
def box(name, size, loc, m, bevel=0.0):
    bpy.ops.mesh.primitive_cube_add(size=1, location=loc); o = bpy.context.object; o.name = name
    o.scale = size; bpy.ops.object.transform_apply(scale=True)
    if bevel:
        mod = o.modifiers.new('b', 'BEVEL'); mod.width = bevel; mod.segments = 3
    o.data.materials.append(m); return o
def arr(o, n, off):
    a = o.modifiers.new('a', 'ARRAY'); a.count = n; a.use_relative_offset = False; a.use_constant_offset = True; a.constant_offset_displace = off

WHITE = mat('board', (0.93, 0.92, 0.88), 0.6); HOLE = mat('hole', (0.05, 0.05, 0.05), 0.9)
PCB = mat('pcb', (0.05, 0.32, 0.12), 0.45); METAL = mat('metal', (0.8, 0.8, 0.82), 0.25, 1.0)
GOLD = mat('gold', (0.9, 0.7, 0.3), 0.3, 1.0); BLACK = mat('black', (0.03, 0.03, 0.03), 0.4)
RED = mat('red', (0.8, 0.08, 0.06), 0.5); BLUE = mat('blue', (0.08, 0.2, 0.75), 0.5)
TERRA = mat('terra', (0.71, 0.31, 0.17), 0.45); LED = mat('led', (0.95, 0.12, 0.02), 0.15, 0, 8.0)
TAN = mat('tan', (0.8, 0.65, 0.45), 0.5); GRAY = mat('ribbon', (0.55, 0.56, 0.58), 0.6)

# Breadboard: 63 columns, two banks of five rows, two power rails each side.
W, D, H = 63 * P + 0.6, 22 * P, 0.85
box('breadboard', (W, D, H), (0, 0, H / 2), WHITE, 0.08)
box('channel', (W - 0.4, 0.3, 0.05), (0, 0, H), HOLE)
x0 = -31 * P
for bank in (-1, 1):
    h = box('holes', (0.1, 0.1, 0.02), (x0, bank * (P * 1.5 + 0.2) - 2 * P, H + 0.002), HOLE)
    h.location.y = bank * 0.35 + (0 if bank > 0 else -4 * P); arr(h, 63, (P, 0, 0)); a2 = h.modifiers.new('a2', 'ARRAY'); a2.count = 5; a2.use_relative_offset = False; a2.use_constant_offset = True; a2.constant_offset_displace = (0, P, 0)
    for r, col in ((0, RED), (1, BLUE)):
        y = bank * (D / 2 - 0.35 - r * P)
        rail = box('rail', (0.1, 0.1, 0.02), (x0 + 2 * P, y, H + 0.002), HOLE); arr(rail, 25, (2 * P, 0, 0))
        box('rail_line', (W - 1.4, 0.04, 0.005), (0, y + bank * 0.2 * (1 if r == 0 else -1), H + 0.003), col)

# Raspberry Pi 4 beside the board.
px, py = -W / 2 + 4.0 + 2.0, D / 2 + 3.4
box('pi_pcb', (8.5, 5.6, 0.14), (px, py, 0.07), PCB, 0.12)
box('soc', (1.5, 1.5, 0.12), (px - 0.6, py - 0.2, 0.2), METAL, 0.02)
box('ram', (1.2, 1.0, 0.1), (px + 1.2, py - 0.2, 0.19), BLACK)
box('eth', (2.1, 1.6, 1.35), (px - 3.25, py - 1.6, 0.8), METAL, 0.04)
box('usb_a', (1.75, 1.3, 1.5), (px - 3.25, py - 0.3, 0.86), METAL, 0.04)
box('usb_b', (1.75, 1.3, 1.5), (px - 3.25, py - 1.9, 0.86), METAL, 0.04)
for i, xo in enumerate((-0.4, 1.0, 2.4)):
    box('port', (0.9 if i else 0.7, 0.75, 0.35), (px + xo, py + 2.55, 0.32), METAL, 0.02)
hdr = box('gpio_header', (20 * P, 2 * P, 0.25), (px + 0.4, py - 2.45, 0.27), BLACK)
pin = box('pin', (0.06, 0.06, 0.6), (px + 0.4 - 9.5 * P, py - 2.45 - P / 2, 0.55), GOLD); arr(pin, 20, (P, 0, 0)); a2 = pin.modifiers.new('a2', 'ARRAY'); a2.count = 2; a2.use_relative_offset = False; a2.use_constant_offset = True; a2.constant_offset_displace = (0, P, 0)

# T-cobbler on the breadboard, terracotta, with a ribbon cable from the Pi's header.
cx = -W / 2 + 4.0
box('cobbler', (20 * P + 0.4, 2.2, 0.35), (cx + 2.4, 0, H + 0.2), TERRA, 0.04)
box('cobbler_socket', (20 * P, 2 * P, 0.5), (cx + 2.4, 0, H + 0.6), BLACK, 0.02)
bpy.ops.curve.primitive_bezier_curve_add(); rb = bpy.context.object; rb.name = 'ribbon'
sp = rb.data.splines[0].bezier_points
sp[0].co = Vector((px + 0.4, py - 2.45, 0.9)); sp[0].handle_left = sp[0].co - Vector((0, 0, 1)); sp[0].handle_right = sp[0].co + Vector((0, -1.2, 0.9))
sp[1].co = Vector((cx + 2.4, 0, H + 1.0)); sp[1].handle_left = sp[1].co + Vector((0, 1.2, 0.9)); sp[1].handle_right = sp[1].co - Vector((0, 0, 1))
bpy.ops.curve.primitive_bezier_curve_add(); prof = bpy.context.object; prof.name = 'ribbon_profile'
pp = prof.data.splines[0].bezier_points; pp[0].co = (-2.6, 0, 0); pp[1].co = (2.6, 0, 0)
for p in pp: p.handle_left_type = p.handle_right_type = 'VECTOR'
prof.data.dimensions = '2D'; prof.hide_render = True; prof.hide_viewport = True
rb.data.bevel_mode = 'OBJECT'; rb.data.bevel_object = prof; rb.data.extrude = 0.02; rb.data.materials.append(GRAY)

def wire(a, b, m, lift=1.4):
    a, b = Vector(a), Vector(b)
    bpy.ops.curve.primitive_bezier_curve_add(); w = bpy.context.object
    s = w.data.splines[0].bezier_points
    s[0].co = a; s[0].handle_left = a - Vector((0, 0, 0.5)); s[0].handle_right = a + Vector((0, 0, lift))
    s[1].co = b; s[1].handle_left = b + Vector((0, 0, lift)); s[1].handle_right = b - Vector((0, 0, 0.5))
    w.data.bevel_depth = 0.045; w.data.bevel_resolution = 4; w.data.materials.append(m)

top = H + 0.02
# LED with resistor, a push button, a little speaker, and a serial header.
lx = 3.0
for dx, m in ((0, LED),):
    bpy.ops.mesh.primitive_cylinder_add(radius=0.25, depth=0.55, location=(lx, 0.9, top + 0.85)); led = bpy.context.object; led.data.materials.append(m)
    bpy.ops.mesh.primitive_uv_sphere_add(radius=0.25, location=(lx, 0.9, top + 1.12)); bpy.context.object.data.materials.append(m)
    for o in (-P / 2, P / 2): box('leg', (0.03, 0.03, 0.6), (lx + o, 0.9, top + 0.3), METAL)
bpy.ops.mesh.primitive_cylinder_add(radius=0.11, depth=0.65, location=(lx + 1.2, 1.1, top + 0.35), rotation=(0, math.pi / 2, 0)); bpy.context.object.data.materials.append(TAN)
for b, c in zip((0.36, 0.5, 0.64), (BLUE, BLACK, RED)):
    bpy.ops.mesh.primitive_cylinder_add(radius=0.115, depth=0.04, location=(lx + 1.2 + (b - 0.5), 1.1, top + 0.35), rotation=(0, math.pi / 2, 0)); bpy.context.object.data.materials.append(c)
box('button', (0.6, 0.6, 0.35), (6.5, -0.9, top + 0.2), BLACK, 0.03)
bpy.ops.mesh.primitive_cylinder_add(radius=0.17, depth=0.15, location=(6.5, -0.9, top + 0.45)); bpy.context.object.data.materials.append(RED)
bpy.ops.mesh.primitive_cylinder_add(radius=1.0, depth=0.35, location=(6.0, 1.3, top + 0.4)); spk = bpy.context.object; spk.data.materials.append(BLACK)
bpy.ops.mesh.primitive_cylinder_add(radius=0.75, depth=0.05, location=(6.0, 1.3, top + 0.6)); bpy.context.object.data.materials.append(METAL)
box('serial', (6 * P, P, 0.25), (-1.6, -1.5, top + 0.13), BLACK)
s = box('serial_pin', (0.06, 0.06, 0.6), (-1.6 - 2.5 * P, -1.5, top + 0.45), GOLD); arr(s, 6, (P, 0, 0))
cob = Vector((cx + 2.4, 0, H + 0.3))
wire((cx + 0.5, -1.6, top), (lx - P / 2, 0.9, top), RED)
wire((cx + 1.0, -1.6, top), (6.3, -0.9, top), BLUE)
wire((cx + 1.5, 1.6, top), (5.4, 1.3, top + 0.2), TERRA, 1.6)
wire((cx + 4.2, -1.6, top), (-1.6, -1.5, top), mat('green', (0.1, 0.55, 0.25), 0.5))
wire((cx + 4.6, 1.6, top), (-0.3, D / 2 - 0.35, top), BLACK, 0.8)

# Desk, light, camera.
box('desk', (80, 60, 1), (0, 0, -0.5), mat('desk', (0.96, 0.95, 0.92), 0.85))
bpy.ops.object.light_add(type='AREA', location=(-6, -10, 22)); L = bpy.context.object; L.data.energy = 22000; L.data.size = 9
bpy.ops.object.light_add(type='AREA', location=(18, 14, 10)); L2 = bpy.context.object; L2.data.energy = 3500; L2.data.size = 10
L2.rotation_euler = (math.radians(-55), math.radians(40), 0)
bpy.ops.object.camera_add(location=(4, -17, 24)); cam = bpy.context.object; sc.camera = cam
cam.data.lens = 55
d = Vector((-1.0, 3.0, 0)) - cam.location; cam.rotation_euler = d.to_track_quat('-Z', 'Y').to_euler()
w = bpy.data.worlds.new('w'); sc.world = w; w.use_nodes = True
w.node_tree.nodes['Background'].inputs[0].default_value = (0.98, 0.97, 0.95, 1); w.node_tree.nodes['Background'].inputs[1].default_value = 0.35
try: sc.render.engine = 'BLENDER_EEVEE_NEXT'
except TypeError: sc.render.engine = 'BLENDER_EEVEE'
sc.render.resolution_x, sc.render.resolution_y = 1800, 1100
sc.view_settings.view_transform = 'AgX' if 'AgX' in [i.identifier for i in bpy.types.ColorManagedViewSettings.bl_rna.properties['view_transform'].enum_items] else 'Filmic'
try:
    sc.eevee.use_raytracing = True; sc.eevee.use_shadows = True
except AttributeError: pass
sc.render.filepath = OUT
bpy.ops.render.render(write_still=True)
