/* The stb_truetype implementation, in its own TU so the STBTT hooks the nuklear vendor TU
   predefines (STBTT_malloc/STBTT_free) stay that TU's business rather than leaking into this one.
 *
 * stb_rect_pack is included declaration-only, for one reason: stb_truetype's own fallback packer
 * has a stbrp_context with no `align` field, while nuklear's - which reads the context this TU's
 * stbtt_PackBegin() writes across the TU boundary - does, and a bake would divide by that zero
 * align. Including the real stb_rect_pack (the same v1.01 nuklear inlines) makes stb_truetype
 * adopt its stbrp_context layout, and the two stbrp calls it makes link to the extern
 * stbrp_init_target / stbrp_pack_rects the nuklear vendor TU already defines - one layout, one
 * implementation, and nothing compiled here that this TU would not call.
*/
#include "stb_rect_pack.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
