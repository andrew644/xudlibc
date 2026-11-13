# TODO

Renderer roadmap: each item only depends on items above it.

## Stage 3 - scale

- **S3.1** Let a texture hold hundreds of images instead of 15, and stop switching image
  sets per object. Must come before the next item, which can't switch anything.
- **S3.2** Draw thousands of copies of the same object in one call (grass, rocks, bullets).
- **S3.3** Let the graphics card decide what's off-screen or hidden and skip it, with no CPU
  involvement. This changes how geometry is stored, so it comes after the two above.

## Stage 4 - expensive polish

- **S4.1** Reuse last frame's data to smooth flickering (motion blur, ambient occlusion,
  reflections). Needs a per-object history.
- **S4.2** Many-lights rendering path, only if the game actually needs dozens of dynamic
  lights.
- **S4.3** Ray-traced reflections and global illumination. Last: most memory, most new
  machinery, fewest players with the hardware.
