# TODO
- Add `xud_set_audio_volume()`/`xud_get_audio_volume()` above the row's own mixer node — raylib's `SetAudioVolume`, and the first thing that needs a node between the row's sound and the endpoint
- Add the footprint half: `xud_set_audio_footprint()` over a per-row spatializer node, replacing the rows' `MA_SOUND_FLAG_NO_SPATIALIZATION` — raylib's `SetAudioFootprint`, still absent above this rung
- Add an example that plays a stream while drawing frames, so the audio thread and the frame loop are seen coexisting rather than assumed to
