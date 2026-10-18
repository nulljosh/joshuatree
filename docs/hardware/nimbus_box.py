# SPDX-License-Identifier: CC-BY-NC-SA-4.0
# Copyright (c) 2026 Joshua Trommel. Hardware design, see docs/hardware/LICENSE-NOTICE.md
# The Nimbus retail box, a real 3D model: a cream matte lidded box, one seam, the tree mark centred on the lid, "Joshua Tree" small beneath, nothing else.
# render_nimbus.py calls label() outside Blender and build() inside it (NIMBUS_BOX=1 python3 docs/hardware/render_nimbus.py).
import os, subprocess, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
BW, BD, BH, LID_H, OVER = 330.0, 300.0, 150.0, 64.0, 1.0      # mm: width, depth, height, lid height, how far the lid overhangs the base per side
LABEL_MM = 170.0                                              # the printed square on the lid

def label(path, px=1700):
    """the lid print: the current brand mark (landing/mark-tree.svg) and the name beneath, transparent background"""
    from PIL import Image, ImageDraw, ImageFont
    mk = os.path.join(tempfile.mkdtemp(), "mark.png")
    subprocess.run(["rsvg-convert", "-h", str(int(px * 0.56)), "-o", mk, os.path.join(HERE, "..", "..", "landing", "mark-tree.svg")], check=True)
    m = Image.open(mk).convert("RGBA")
    im = Image.new("RGBA", (px, px), (0, 0, 0, 0)); im.paste(m, ((px - m.width) // 2, int(px * 0.12)), m)
    d = ImageDraw.Draw(im); f = ImageFont.truetype("/System/Library/Fonts/Helvetica.ttc", int(px * 0.05))
    txt, sp = "Joshua Tree", int(px * 0.008)
    tw = sum(d.textlength(c, font=f) + sp for c in txt) - sp; x = (px - tw) / 2; y = int(px * 0.12) + m.height + int(px * 0.07)
    for c in txt:
        d.text((x, y), c, font=f, fill=(0x5A, 0x56, 0x4F, 255)); x += d.textlength(c, font=f) + sp
    im.save(path)

def build(bpy, M, label_png):
    """cream matte base and lid with a 1 mm overhang seam, the print on the lid; sizes in metres (the scene is 0.001 scale)"""
    import bmesh
    S = 0.001
    def block(name, w, d, z0, z1):
        bm = bmesh.new(); bmesh.ops.create_cube(bm, size=1.0)
        me = bpy.data.meshes.new(name); bm.to_mesh(me); bm.free()
        o = bpy.data.objects.new(name, me); bpy.context.scene.collection.objects.link(o)
        o.scale = (w * S, d * S, (z1 - z0) * S); o.location = (0, 0, (z0 + z1) / 2 * S)
        bev = o.modifiers.new("bevel", "BEVEL"); bev.width = 0.0022; bev.segments = 4; bev.limit_method = "ANGLE"
        bpy.context.view_layer.objects.active = o
        o.data.materials.append(M["cream"])
        for p in o.data.polygons: p.use_smooth = True
        bpy.ops.object.select_all(action="DESELECT"); o.select_set(True)
        bpy.ops.object.transform_apply(scale=True)           # keep the bevel the same size on every edge
        return o
    base = block("base", BW - 2 * OVER, BD - 2 * OVER, 0.0, BH - LID_H + 8.0)
    lid = block("lid", BW, BD, BH - LID_H, BH)
    # the print: a plane just above the lid top, the PNG's alpha cuts it to the mark and the name
    me = bpy.data.meshes.new("print"); h = LABEL_MM / 2 * S; z = BH * S + 0.00005
    me.from_pydata([(-h, -h, z), (h, -h, z), (h, h, z), (-h, h, z)], [], [(0, 1, 2, 3)]); uv = me.uv_layers.new(name="uv")
    for i, c in enumerate(((0, 0), (1, 0), (1, 1), (0, 1))): uv.data[i].uv = c
    o = bpy.data.objects.new("print", me); bpy.context.scene.collection.objects.link(o)
    m = bpy.data.materials.new("print"); m.use_nodes = True; nt = m.node_tree
    b = next(n for n in nt.nodes if n.type == "BSDF_PRINCIPLED"); t = nt.nodes.new("ShaderNodeTexImage"); t.image = bpy.data.images.load(label_png)
    nt.links.new(t.outputs["Color"], b.inputs["Base Color"]); nt.links.new(t.outputs["Alpha"], b.inputs["Alpha"])
    b.inputs["Roughness"].default_value = 0.62
    for k in ("Specular IOR Level", "Specular"):
        if k in b.inputs: b.inputs[k].default_value = 0.15
    me.materials.append(m)
    return [base, lid, o]
