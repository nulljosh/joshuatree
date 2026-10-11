# Renders docs/hardware/samantha-box-*.jpg: the Samantha box from the STLs samantha_cad.py writes, exploded.
# Run: python3 docs/hardware/samantha_cad.py /tmp/box && blender -b --factory-startup -P tools/gen/samantha_box_render.py -- /tmp/box OUTDIR
import bpy, sys
a = sys.argv[sys.argv.index("--") + 1:]
SRC, DST = a[0].rstrip("/") + "/", a[1].rstrip("/") + "/"
import math
from mathutils import Vector
d = SRC
bpy.ops.wm.read_factory_settings(use_empty=True)
cols = {"back_shell": (0.71, 0.31, 0.17, 1), "front_bezel": (0.93, 0.91, 0.87, 1)}
for n, off in (("back_shell", 0), ("front_bezel", 22)):
    bpy.ops.wm.stl_import(filepath=d + n + ".stl")
    o = bpy.context.selected_objects[0]; o.name = n; o.location.z += off
    m = bpy.data.materials.new(n); m.diffuse_color = cols[n]; o.data.materials.append(m)
sc = bpy.context.scene
sc.render.engine = "BLENDER_WORKBENCH"
sc.display.shading.light = "STUDIO"; sc.display.shading.color_type = "MATERIAL"
sc.display.shading.show_cavity = True
sc.render.resolution_x, sc.render.resolution_y = 1400, 900
w = bpy.data.worlds.new("w"); sc.world = w; w.color = (0.98, 0.97, 0.96)
cam = bpy.data.objects.new("c", bpy.data.cameras.new("c")); sc.collection.objects.link(cam); sc.camera = cam
def shot(name, loc, tgt=(0, 0, 14)):
    cam.location = loc
    cam.rotation_euler = (Vector(tgt) - Vector(loc)).to_track_quat("-Z", "Y").to_euler()
    sc.render.filepath = DST + f"samantha-box-{name}.png"; bpy.ops.render.render(write_still=True)
shot("back", (-150, -190, -140), (0, 0, 10))
shot("front", (150, -230, 210))
