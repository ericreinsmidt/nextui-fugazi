// Fugazi CRT - Pass 1: Barrel distortion + soft glow
// Optimized for Mali-G31 (TrimUI Brick)

#pragma parameter CURVATURE "Curvature" 0.06 0.0 0.15 0.01
#pragma parameter GLOW_MIX "Glow Strength" 0.35 0.0 0.6 0.05

#if defined(VERTEX)

uniform mediump mat4 MVPMatrix;
attribute mediump vec4 VertexCoord;
attribute mediump vec2 TexCoord;

uniform mediump vec2 TextureSize;

varying mediump vec2 TEX0;
varying mediump vec2 v_offset_x;
varying mediump vec2 v_offset_y;

void main()
{
    TEX0 = TexCoord;
    gl_Position = MVPMatrix * VertexCoord;

    // Precompute blur offsets in vertex shader
    // 2.0 texel offset with LINEAR filtering gives a wide soft tap
    v_offset_x = vec2(2.0 / TextureSize.x, 0.0);
    v_offset_y = vec2(0.0, 2.0 / TextureSize.y);
}

#elif defined(FRAGMENT)

#ifdef GL_ES
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
#endif

uniform sampler2D Texture;
uniform vec2 InputSize;
uniform vec2 TextureSize;

varying vec2 TEX0;
varying vec2 v_offset_x;
varying vec2 v_offset_y;

#ifdef PARAMETER_UNIFORM
uniform float CURVATURE;
uniform float GLOW_MIX;
#else
#define CURVATURE 0.06
#define GLOW_MIX 0.35
#endif

void main()
{
    // --- Barrel distortion (quadratic, no trig) ---
    vec2 scale = TextureSize / InputSize;
    vec2 tex0 = TEX0 * scale;
    vec2 centered = tex0 - vec2(0.5);
    float r2 = dot(centered, centered);
    centered *= 1.0 + CURVATURE * r2;

    // Clip outside curved screen
    vec2 edge = step(vec2(-0.5), centered) * step(centered, vec2(0.5));
    float inside = edge.x * edge.y;

    vec2 uv = (centered + vec2(0.5)) / scale;

    // --- Soft glow via 5-tap cross blur ---
    // Center + 4 neighbors, bilinear filtering does the heavy lifting
    vec3 center = texture2D(Texture, uv).rgb;
    vec3 blur  = texture2D(Texture, uv + v_offset_x).rgb;
         blur += texture2D(Texture, uv - v_offset_x).rgb;
         blur += texture2D(Texture, uv + v_offset_y).rgb;
         blur += texture2D(Texture, uv - v_offset_y).rgb;
    blur *= 0.25;

    // Mix sharp center with soft blur for glow
    vec3 color = mix(center, blur, GLOW_MIX);

    gl_FragColor = vec4(color * inside, 1.0);
}

#endif
