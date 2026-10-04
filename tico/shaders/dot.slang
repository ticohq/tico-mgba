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
const float gamma = 2.4;
const float shine = 0.05;
const float blend = 0.65;

float pixdist(vec2 coord, vec2 source) {
    vec2 delta = coord - source;
    return sqrt(dot(delta, delta));
}

float color_bloom(vec3 color) {
    const vec3 gray_coeff = vec3(0.30, 0.59, 0.11);
    float bright = dot(color, gray_coeff);
    return mix(1.0 + shine, 1.0 - shine, bright);
}

vec3 lookup(vec2 pixel_no, float offset_x, float offset_y, vec3 color) {
    vec2 offset = vec2(offset_x, offset_y);
    float delta = pixdist(fract(pixel_no), offset + vec2(0.5, 0.5));
    return color * exp(-gamma * delta * color_bloom(color));
}

void main() {
    vec2 pixel_no = TexCoord * TextureSize;
    vec2 texel = 1.0 / TextureSize;
    vec3 mid_color = lookup(pixel_no, 0.0, 0.0, texture(Source, TexCoord).rgb);
    vec3 color = vec3(0.0);
    color += lookup(pixel_no, -1.0, -1.0, texture(Source, TexCoord + vec2(-texel.x, -texel.y)).rgb);
    color += lookup(pixel_no,  0.0, -1.0, texture(Source, TexCoord + vec2(0.0, -texel.y)).rgb);
    color += lookup(pixel_no,  1.0, -1.0, texture(Source, TexCoord + vec2(texel.x, -texel.y)).rgb);
    color += lookup(pixel_no, -1.0,  0.0, texture(Source, TexCoord + vec2(-texel.x, 0.0)).rgb);
    color += mid_color;
    color += lookup(pixel_no,  1.0,  0.0, texture(Source, TexCoord + vec2(texel.x, 0.0)).rgb);
    color += lookup(pixel_no, -1.0,  1.0, texture(Source, TexCoord + vec2(-texel.x, texel.y)).rgb);
    color += lookup(pixel_no,  0.0,  1.0, texture(Source, TexCoord + vec2(0.0, texel.y)).rgb);
    color += lookup(pixel_no,  1.0,  1.0, texture(Source, TexCoord + vec2(texel.x, texel.y)).rgb);
    vec3 out_color = mix(1.2 * mid_color, color, blend);
    FragColor = vec4(out_color, 1.0);
}
