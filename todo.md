# TODO

Renderer roadmap: each item only depends on items above it.

## Stage 1 - plumbing (invisible, but everything later pays for skipping it)

- **S1.3** One central place that tracks what each image is being used for right now (a
  target? a source? being copied?). Every new effect needs this, and hand-writing it per
  effect causes rare crashes that are miserable to trace later.
- **S1.4** Allow drawing into off-screen images, not just the window. Shadows, glow, motion
  blur and reflections all need scratch images. This is the single biggest unlock.

## Stage 2 - how the picture is built

- **S2.1** Draw into a high-precision scratch image, then convert it to the screen at the
  end. Do this before lighting, because it changes what brightness values look right;
  tuning lights twice is wasted work.
- **S2.2** Real lights: point, spotlight, sun. Until now the shader just fakes one fixed
  light.
- **S2.3** Fix depth precision so nearby and distant objects both sort correctly. Do this
  before shadows - shadows compare against depth, so the depth format has to be settled
  first.
- **S2.4** A cheap "who's in front" pass first, so hidden surfaces aren't shaded twice.
- **S2.5** Shadows.
- **S2.6** Glow/bloom and edge-smoothing.

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
