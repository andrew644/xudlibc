# TODO
- Add the footprint half: `xud_set_audio_footprint()` over a per-row spatializer node, replacing the rows' `MA_SOUND_FLAG_NO_SPATIALIZATION` — raylib's `SetAudioFootprint`, still absent above this rung
- Add an example that plays a stream while drawing frames, so the audio thread and the frame loop are seen coexisting rather than assumed to
