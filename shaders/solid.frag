#version 460 core

/* solid.frag - the untextured-solid variant, lit by one fixed direction.

   The interpolated colour is no longer written straight to the swapchain image: it is
   modulated by a Lambert term against ONE fixed light direction, and the interpolated
   normal at location 2 is what makes that term exist at all. The vertex stage has been
   outputting that normal since the batch landed and nothing read it, which is why every
   face of a box came out at one flat colour and a box read as a flat hexagonal silhouette
   rather than as a solid.

   The normal is read at location 2, which is the location xud_batch_fill_attribute gives
   the NORMAL stream's attribute and the vertex stage's outNormal the location that
   follows COLOR. A frag input's location must name a vertex output, and the validation
   layer refuses one that does not, so the pair is a fact rather than a convention.

   The normal is renormalized here rather than trusted. A box's four vertices of one face
   share that face's normal, so the interpolated vector is already unit for a box, but a
   sphere's three vertices of one triangle carry three different unit normals and the
   interpolated vector is SHORTER than one - which is what would darken the middle of
   every one of a sphere's triangles against its own corners. Every shape the shapes rung
   appends writes a UNIT normal, so the input is never the zero vector and normalize()
   cannot divide by zero.

   The light is a constant in this stage rather than a member of set 0's block: the block
   declares ambient and lightCount, but the descriptor rung zeroes them and nothing fills
   them, so a shader that read them would be a shader that reads zero. Promoting the
   direction and the ambient floor into set 0 is the fog / lighting step's, and it is
   recorded as open in todo.md rather than half-done here.

   The ambient floor is not decoration: the pass clears the color attachment to (0.02,
   0.02, 0.05), so a face turned away from a single light at 0 would be pure black on
   near-black, a face that is drawn but not seen. */

layout(location = 1) in vec4 inColor;
layout(location = 2) in vec3 inNormal;
layout(location = 0) out vec4 outColor;

/* The direction points FROM the surface TOWARD the light, which is why the term below is
   a plain dot(): a face whose unit normal is aligned with this vector is fully lit and
   one at or past 90 degrees to it keeps only the floor below. It is normalize(vec3(1.0,
   2.0, 3.0)) written out as components - normalize() is not a constant expression, and
   this constant is written once. World-space: the model matrix is identity for every
   shape the shapes rung appends today, so the attribute's normal is already in this
   space, and a non-identity model would need the vertex stage to carry the normal over
   instead. */
const vec3 xudLightDir = vec3(0.267261f, 0.534522f, 0.801784f);

const float xudAmbient = 0.25f;

void main()
{
    vec3 normal = normalize(inNormal);
    float lambert = max(dot(normal, xudLightDir), 0.0f);
    float shade = min(xudAmbient + lambert, 1.0f);
    outColor = vec4(inColor.rgb * shade, inColor.a);
}
