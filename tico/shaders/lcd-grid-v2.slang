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
#define RSUBPIX_R 1.0
#define RSUBPIX_G 0.0
#define RSUBPIX_B 0.0
#define GSUBPIX_R 0.0
#define GSUBPIX_G 1.0
#define GSUBPIX_B 0.0
#define BSUBPIX_R 0.0
#define BSUBPIX_G 0.0
#define BSUBPIX_B 1.0
#define u_gain 1.5
#define u_gamma 2.3
#define u_blacklevel 0.0
#define u_ambient 0.0
#define outgamma 2.2

float intsmear_func(float z, float coeffs[7]) {
    float z2 = z*z;
    float zn = z;
    float ret = 0.0;
    for (int i = 0; i < 7; i++) {
        ret += zn*coeffs[i];
        zn *= z2;
    }
    return ret;
}

float intsmear(float x, float dx, float d, float coeffs[7]) {
    float zl = clamp((x-dx*0.5)/d,-1.0,1.0);
    float zh = clamp((x+dx*0.5)/d,-1.0,1.0);
    return d * ( intsmear_func(zh,coeffs) - intsmear_func(zl,coeffs) )/dx;
}

void main() {
    float coeffs_x[7] = float[7](1.0, -2.0/3.0, -1.0/5.0, 4.0/7.0, -1.0/9.0, -2.0/11.0, 1.0/13.0);
    float coeffs_y[7] = float[7](1.0, 0.0, -4.0/5.0, 2.0/7.0, 4.0/9.0, -4.0/11.0, 1.0/13.0);
    vec2 texelSize = 1.0 / TextureSize;

    vec3 outgamma3 = vec3(outgamma);
    vec3 cred   = pow(vec3(RSUBPIX_R, RSUBPIX_G, RSUBPIX_B), outgamma3);
    vec3 cgreen = pow(vec3(GSUBPIX_R, GSUBPIX_G, GSUBPIX_B), outgamma3);
    vec3 cblue  = pow(vec3(BSUBPIX_R, BSUBPIX_G, BSUBPIX_B), outgamma3);

    ivec2 tli = ivec2(floor(TexCoord/texelSize - vec2(0.4999)));

    vec3 lcol, rcol;
    float subpix = (TexCoord.x/texelSize.x - 0.4999 - float(tli.x))*3.0;
    float rsubpix = 3.0 / TextureSize.x;
    lcol = vec3(intsmear(subpix+1.0,rsubpix, 1.5, coeffs_x),
               intsmear(subpix    ,rsubpix, 1.5, coeffs_x),
               intsmear(subpix-1.0,rsubpix, 1.5, coeffs_x));
    rcol = vec3(intsmear(subpix-2.0,rsubpix, 1.5, coeffs_x),
               intsmear(subpix-3.0,rsubpix, 1.5, coeffs_x),
               intsmear(subpix-4.0,rsubpix, 1.5, coeffs_x));

    float tcol, bcol;
    subpix = TexCoord.y/texelSize.y - 0.4999 - float(tli.y);
    rsubpix = 1.0/TextureSize.y;
    tcol = intsmear(subpix    ,rsubpix, 0.63, coeffs_y);
    bcol = intsmear(subpix-1.0,rsubpix, 0.63, coeffs_y);

    vec3 gain3 = vec3(u_gain);
    vec3 gamma3 = vec3(u_gamma);
    vec3 blacklevel3 = vec3(u_blacklevel);
    vec3 ambient3 = vec3(u_ambient);
    vec3 tcol3 = vec3(tcol);
    vec3 bcol3 = vec3(bcol);

    vec3 p00 = texture(Source, TexCoord - 0.25 * texelSize * vec2(0.0, 0.0)).rgb;
    vec3 p11 = texture(Source, TexCoord - 0.25 * texelSize * vec2(1.0, 1.0)).rgb;
    vec3 p01 = texture(Source, TexCoord - 0.25 * texelSize * vec2(0.0, 1.0)).rgb;
    vec3 p10 = texture(Source, TexCoord - 0.25 * texelSize * vec2(1.0, 0.0)).rgb;

    vec3 topLeftColor     = (pow(gain3 * p00 + blacklevel3, gamma3) + ambient3) * lcol * tcol3;
    vec3 bottomRightColor = (pow(gain3 * p11 + blacklevel3, gamma3) + ambient3) * rcol * bcol3;
    vec3 bottomLeftColor  = (pow(gain3 * p01 + blacklevel3, gamma3) + ambient3) * lcol * bcol3;
    vec3 topRightColor    = (pow(gain3 * p10 + blacklevel3, gamma3) + ambient3) * rcol * tcol3;

    vec3 averageColor = topLeftColor + bottomRightColor + bottomLeftColor + topRightColor;
    averageColor = mat3(cred.x, cred.y, cred.z,
                        cgreen.x, cgreen.y, cgreen.z,
                        cblue.x, cblue.y, cblue.z) * averageColor;
    FragColor = vec4(pow(averageColor, vec3(1.0/outgamma)), 1.0);
}
