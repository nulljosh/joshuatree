# SPDX-License-Identifier: CC-BY-NC-SA-4.0
# Copyright (c) 2026 Joshua Trommel. Hardware design, see docs/hardware/LICENSE-NOTICE.md
# Samantha box preview renders, from the real CAD: samantha-box-front.jpg and samantha-box-back.jpg.
#   python3 docs/hardware/render_samantha.py [outdir]     (default docs/hardware)
# Step 1 builds samantha_cad.py (all its asserts) into a temp folder. Step 2 runs Blender headless on the shell
# and the bezel, lifted off it, in Workbench: quick flat previews, not product shots. Step 3 shrinks the two
# PNGs to 1100 px JPGs with magick. Nothing but the JPGs lands in outdir.
import sys, os, subprocess, tempfile, shutil

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)                                    # Blender's -P does not put this folder on the path
from render_kit import build, blender, argv, material, aim  # shared with render_neo.py

SHELL, BEZEL = (0xB5, 0x50, 0x2C), (0xE9, 0xE0, 0xCF)       # terracotta and cream
PAPER = "#faf8f4"                                           # the page colour the JPGs are flattened onto
EV = 0.6                                                    # lifts the studio-lit faces back toward the filament colour
W, H, JPG_W = 1400, 900, 1100
LIFT = 22.0                                                 # how far the bezel floats above the shell, exploded
VIEWS = [("front", (165, -255, 230), (0, 0, 14)),           # name, camera, target (mm)
         ("back", (-185, -235, -175), (0, 0, 10))]

try:
    import bpy
except ImportError:
    bpy = None

if bpy is None:
    out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else HERE)
    tmp = tempfile.mkdtemp(prefix="samantha-box-")
    try:
        build("samantha_cad.py", tmp)
        blender(__file__, tmp, factory=True)
        for name, _, _ in VIEWS:
            subprocess.run(["magick", os.path.join(tmp, name + ".png"), "-background", PAPER, "-flatten", "-resize", f"{JPG_W}x",
                            "-strip", "-quality", "82", os.path.join(out, f"samantha-box-{name}.jpg")], check=True)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    sys.exit(0)

src = argv()[0]
bpy.ops.wm.read_factory_settings(use_empty=True)
for name, rgb, lift in (("back_shell", SHELL, 0.0), ("front_bezel", BEZEL, LIFT)):
    bpy.ops.wm.stl_import(filepath=os.path.join(src, name + ".stl"))
    o = bpy.context.selected_objects[0]; o.name = name; o.location.z += lift
    o.data.materials.append(material(name, rgb))
sc = bpy.context.scene
sc.render.engine = "BLENDER_WORKBENCH"
sc.display.shading.light = "STUDIO"; sc.display.shading.color_type = "MATERIAL"
sc.display.shading.show_cavity = True
try: sc.view_settings.view_transform = "Standard"           # no tone-mapper hue shift on the filament colours
except TypeError: pass
sc.view_settings.exposure = EV
sc.render.film_transparent = True                           # magick lays the page colour in behind
sc.render.image_settings.color_mode = "RGBA"
sc.render.resolution_x, sc.render.resolution_y, sc.render.resolution_percentage = W, H, 100
cam_d = bpy.data.cameras.new("c"); cam_d.clip_end = 5000.0
cam = bpy.data.objects.new("c", cam_d); sc.collection.objects.link(cam); sc.camera = cam
for name, loc, target in VIEWS:
    aim(cam, loc, target)
    sc.render.filepath = os.path.join(src, name + ".png"); bpy.ops.render.render(write_still=True)
