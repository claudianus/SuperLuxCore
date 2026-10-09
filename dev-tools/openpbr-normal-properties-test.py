"""New lobe-normal properties survive export, texture edits and reparse."""
import importlib.metadata
import pysuperluxcore as native

assert native.Version() == importlib.metadata.version("pysuperluxcore")
for imported, correction, use_coat in (
    (False, False, False), (True, True, True), (True, False, True)
):
    props = native.Properties()
    props.SetFromString("""
scene.textures.coat_input.type = normalvector
scene.textures.coat_input.texture = 0.2 0.0 0.98
scene.materials.surface.type = openpbr
scene.materials.surface.basecolor = 0.4 0.2 0.1
""")
    prefix = "scene.materials.surface."
    if imported:
        props.Set(native.Property(prefix + "cyclesnormalsemantics", imported))
        props.Set(native.Property(prefix + "reflectionnormalcorrection", correction))
    if use_coat:
        props.Set(native.Property(prefix + "coatnormal", "coat_input"))
    scene = native.Scene()
    scene.Parse(props)
    # Redefining a referenced texture must retain the material's new pointer.
    update = native.Properties()
    update.SetFromString("""
scene.textures.coat_input.type = normalvector
scene.textures.coat_input.texture = 0.0 0.3 0.95
""")
    scene.Parse(update)
    exported = scene.ToProperties()
    clone = native.Scene()
    serialized = native.Properties()
    serialized.SetFromString(exported.ToString())
    clone.Parse(serialized)
    reparsed = clone.ToProperties()
    for result in (exported, reparsed):
        assert result.Get(prefix + "cyclesnormalsemantics").GetBool() == imported
        assert result.Get(prefix + "reflectionnormalcorrection").GetBool() == correction
        assert result.IsDefined(prefix + "coatnormal") == use_coat
        if use_coat:
            assert result.Get(prefix + "coatnormal").GetString() == "coat_input"
    print("OPENPBR_NORMAL_PROPERTIES_PASS", imported, correction, use_coat, flush=True)
