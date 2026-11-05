# TODO
- Add xud_get_font_atlas_dim(): the atlas extent is only in the internal record today
- Add `xud_set_audio_volume()`/`xud_get_audio_volume()` above the row's own mixer node — raylib's `SetAudioVolume`, and the first thing that needs a node between the row's sound and the endpoint
- Add the footprint half: `xud_set_audio_footprint()` over a per-row spatializer node, replacing the rows' `MA_SOUND_FLAG_NO_SPATIALIZATION` — raylib's `SetAudioFootprint`, still absent above this rung
- Add an example that plays a stream while drawing frames, so the audio thread and the frame loop are seen coexisting rather than assumed to
- Add dear imgui / microui / nuklear support

## Textured models — the texture step, in dependency order
- Give `XudMaterial` a diffuse-map field (raylib's `MATERIAL_DIFFUSE_MAP` shape): the description is `{ uint32_t blendMode }` today, `xud_material_open()` must stash the texture handle, and a material that names a map must draw under `XUD_BATCH_VARIANT_TEXTURED` instead of opaque/blend
- Get a real texture into a set 1 slot: the 16-entry sampler array is written once, all entries at the white default, so something must run `vkUpdateDescriptorSets` with a texture's imageView/sampler into a slot — a free-list over the 16, written before the slot is ever bound; closing a texture must not rewrite a descriptor an in-flight frame may still read
- Bind the draw's slot in the frame loop: `solid_textured.frag` declares ONE `sampler2D`, so the slot is chosen by `vkCmdBindDescriptorSets` with `descriptorCount 1` and `firstDescriptorIndex textureIndex` — the loop binds the whole array at index 0 once per frame today; add a rebind-on-change branch beside the existing `geometryIndex != boundGeometry` one
- Carry the texture index to the draw: the batch's draw rows have no `textureIndex` member, the record calls take none, and `xud_frame.c` hardcodes `outRow->textureIndex = 0u` — row field, record params, and `xud_draw_mesh()`/`xud_draw_model()` reading the material's texture round this out, and the push row's `textureIndex`/`materialIndex` stop being pushed-as-zero
- Decode glTF images: `xud_model.c` reads materials' alpha mode only and never touches `data->images[]`/`textures[]`; decoding needs an image decoder the repo does not have (stb_image — the user is asked to include it), then per glTF texture decode → `xud_texture_open()` → the texture handle into that model's material slot, closed in the model's rollback and teardown
- Map the glTF sampler: `cgltf_texture_sampler`'s filter and wrap onto `XUD_TEXTURE_*` tokens, or the rung documents that every glTF texture comes in linear/repeat
- Test the textured row deterministically: a fixed-pixel texture, a quad drawn through the textured variant, a readback that proves the texel shows through; and a model example that lands textured material (extend `hello_model.c` or a new example)
