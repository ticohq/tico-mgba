#version 450
// Ported from tico's built-in OpenGL shader of the same name.

layout(push_constant) uniform Push
{
    vec4 SourceSize;
    vec4 OutputSize;
} params;

layout(std140, set = 0, binding = 0) uniform UBO
{
    mat4 MVP;
} global;

#pragma stage vertex
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
layout(location = 0) out vec2 vTexCoord;

void main()
{
    gl_Position = global.MVP * Position;
    vTexCoord = TexCoord;
}

#pragma stage fragment
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 2) uniform sampler2D Source;

#define TexCoord vTexCoord
#define TextureSize params.SourceSize.xy
#define OutputSize params.OutputSize.xy
void main() {
    vec4 color = texture(Source, TexCoord);
    vec2 fragCoord = TexCoord * OutputSize;
    float gridX = mod(fragCoord.x, 3.0);
    float gridY = mod(fragCoord.y, 3.0);
    vec3 mask = vec3(1.0);
    if (gridX < 1.0)       mask = vec3(1.0, 0.7, 0.7);
    else if (gridX < 2.0)  mask = vec3(0.7, 1.0, 0.7);
    else                   mask = vec3(0.7, 0.7, 1.0);
    if (gridY < 1.0) mask *= 0.85;
    FragColor = vec4(color.rgb * mask, color.a);
}
