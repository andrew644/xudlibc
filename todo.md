# TODO
- Allocate and write set 1's sampler array (xud_descriptor_write_samplers above the texture rung)
- Add shaders/solid_textured.frag (tint x texel) and raise the spv header's SPV_COUNT to 3
- Grow the batch's variant axis to name a textured pipeline row in xud_pipeline_fill
- Add miniaudio support
- Add imgui support
- Add LICENSE.md
- Add xud_draw_text()/xud_draw_text_ex() above the font rung, writing real UVs
- Add xud_get_font_atlas_dim(): the atlas extent is only in the internal record today
- Make the shapes append a real uv: a glyph quad needs the atlas box divided by the extent
