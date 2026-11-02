Read this before touching anything here. **Work only inside `/home/andrew/repos/xudlibc`**

## What xudlibc is

- A **C11** library modelled on raylib's public API, but rendered with **Vulkan instead of OpenGL**.
- Same shape as raylib: window/surface creation, `BeginDrawing`/`EndDrawing`, draw calls (`DrawCube`, `DrawMesh`, `DrawTriangle3D`, …), camera + transform math, textures, fonts, models, audio, input — implemented on explicit Vulkan pipelines.
- raylib is a **reference for API shape only** (names, semantics, module split). Never link it, never `#include` it.
  The headers are **already here**: `reference/raylib/raylib.h` If you need a library that raylib uses. Ask the user to include it. Don't build it yourself.

## C house style

- **C11**, one public header (`xud.h`) plus `xud*.h`/`xud*.c` module headers/sources. No C++ anywhere.
- Names: `xudPascalCaseOrNot` — concretely: **functions `xudSnakeCase`** (`xud_init_window`, `xud_draw_cube`), **types `PascalCase`** (`XudWindow`, `XudCamera3D`), **macros/constants `XUD_SNAKE_UPPER`** (`XUD_MAX_LIGHTS`). No Hungarian prefixes.
- All blocks must have curly braces. Even one line if statements.
- Use clang-format before commit.
- Don't add comments. Only add short one lines comments for public methods. (xud.h)

## Vulkan rules (best practices — these are the point of the repo)

- **The API floor is `VK_API_VERSION_1_3`** 

## Testing rules

- Tests must be deterministic — seed any RNG from a fixed test seed.
- You must update tests when you modify code in src/xud.
- Examples don't need tests.

## Working rules

- When you complete a todo item, delete the line in todo.md
- You can't modify AGENTS.md
- When you complete a task suggest items to add to todo.md. Don't add them yourself.
- When you complete a task commit it.
