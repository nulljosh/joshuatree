# SPDX-License-Identifier: CC-BY-NC-SA-4.0
# Copyright (c) 2026 Joshua Trommel. Hardware design, see docs/hardware/LICENSE-NOTICE.md
# Shared by render_nimbus.py and render_samantha.py: build the CAD with uv, then run Blender headless on its meshes.
# Outside Blender only subprocess is used. The bpy helpers import bpy when they are called, inside Blender.
# Blender's -P does not put this folder on sys.path, so each render script adds it before importing this.
import os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
BLENDER = "/opt/homebrew/bin/blender"

def build(script, out, env=None, extra=()):
    """Run a CAD script from this folder into out, with build123d (and any extra --with packages). Its asserts run first."""
    subprocess.run(["uv", "run", "--with", "build123d", *extra, "python", os.path.join(HERE, script), out], check=True, env=env)

def blender(script, *args, factory=False):
    """Run script inside Blender, headless. The script reads its own arguments with argv()."""
    subprocess.run([BLENDER, "-b", *(["--factory-startup"] if factory else []), "-P", script, "--", *args], check=True)

def argv():
    """Inside Blender: the arguments after "--"."""
    return sys.argv[sys.argv.index("--") + 1:]

def lin(c):
    """An sRGB colour, 0 to 255, as the linear RGBA Blender's colour inputs take."""
    f = lambda v: (v / 255.0 / 12.92) if v / 255.0 <= 0.04045 else (((v / 255.0) + 0.055) / 1.055) ** 2.4
    return (f(c[0]), f(c[1]), f(c[2]), 1.0)

def material(name, rgb, rough=0.55):
    """A matte filament: the Principled base colour for Cycles, and the same colour for Workbench renders."""
    import bpy
    m = bpy.data.materials.new(name); m.use_nodes = True
    b = next(n for n in m.node_tree.nodes if n.type == "BSDF_PRINCIPLED")
    b.inputs["Base Color"].default_value = lin(rgb); b.inputs["Roughness"].default_value = rough
    b.inputs["Metallic"].default_value = 0.0
    m.diffuse_color = lin(rgb)
    return m

def aim(obj, loc, target):
    """Put a camera or a light at loc, looking at target."""
    from mathutils import Vector
    obj.location = loc
    obj.rotation_euler = (Vector(target) - Vector(loc)).to_track_quat("-Z", "Y").to_euler()
