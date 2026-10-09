#pragma once

// HLSL compiled at startup with D3DCompile.
inline constexpr char kShaderSource[] = R"hlsl(
cbuffer Params : register(b0)
{
    float4 rect;        // NDC: left, top, right, bottom
    float2 resolution;  // render target size in pixels
    float  time;
    float  bgAlpha;
    float  brightness;
    float  contrast;
    float  saturation;
    float  hue;         // radians
    float  gamma;
    float  sharpness;
    float  scanlines;   // 0..1
    float  curvature;   // 0..1
    float2 texel;       // 1 / video size
    float  power;       // CRT turn-on, 0..1
    float  pad;
    float4 pBgLow;
    float4 pBgHigh;
    float4 pGlow;
    float4 pLight;
    float4 pAccent;
};

Texture2D tex0 : register(t0);
SamplerState samp : register(s0);

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID)
{
    float2 t = float2(id & 1, id >> 1);
    VSOut o;
    o.pos = float4(lerp(rect.x, rect.z, t.x), lerp(rect.y, rect.w, t.y), 0, 1);
    o.uv = t;
    return o;
}

float Hash21(float2 p)
{
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}

float Noise(float2 p)
{
    float2 i = floor(p), f = frac(p);
    float a = Hash21(i), b = Hash21(i + float2(1, 0)), c = Hash21(i + float2(0, 1)), d = Hash21(i + float2(1, 1));
    float2 u = f * f * (3 - 2 * f);
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

float Fbm(float2 p)
{
    float v = 0, a = 0.5;
    [unroll] for (int i = 0; i < 5; ++i)
    {
        v += a * Noise(p);
        p = p * 2.03 + 17.1;
        a *= 0.5;
    }
    return v;
}

// ---- Video with picture controls ----------------------------------------

float3 AdjustColor(float3 c)
{
    c += brightness;
    c = (c - 0.5) * contrast + 0.5;
    // Hue rotation and saturation in YIQ space.
    float y = dot(c, float3(0.299, 0.587, 0.114));
    float i = dot(c, float3(0.596, -0.274, -0.322));
    float q = dot(c, float3(0.211, -0.523, 0.312));
    float ch = cos(hue), sh = sin(hue);
    float i2 = (i * ch - q * sh) * saturation;
    float q2 = (i * sh + q * ch) * saturation;
    c = float3(y + 0.956 * i2 + 0.621 * q2, y - 0.272 * i2 - 0.647 * q2, y - 1.106 * i2 + 1.703 * q2);
    return pow(saturate(c), 1.0 / gamma);
}

float4 PSVideo(VSOut i) : SV_Target
{
    float3 c = tex0.Sample(samp, i.uv).rgb;
    if (sharpness > 0)
    {
        float3 blur = (tex0.Sample(samp, i.uv + float2(texel.x, 0)).rgb + tex0.Sample(samp, i.uv - float2(texel.x, 0)).rgb +
                       tex0.Sample(samp, i.uv + float2(0, texel.y)).rgb + tex0.Sample(samp, i.uv - float2(0, texel.y)).rgb) * 0.25;
        c += (c - blur) * sharpness * 2.0;
    }
    return float4(AdjustColor(c), 1);
}

// ---- PS2-style background: blue void, fog, light towers, particles --------

// ---- Background: slowly morphing topographic contour lines --------------------

float4 PSBackground(VSOut i) : SV_Target
{
    float2 uv = i.uv;
    float aspect = resolution.x / resolution.y;
    float2 p = float2((uv.x - 0.5) * aspect, uv.y - 0.5);

    float3 col = lerp(pBgLow.rgb, pBgHigh.rgb, smoothstep(0.0, 0.95, uv.y));

    // Soft glow behind the content.
    float2 g = (uv - float2(0.5, 0.62)) * float2(1.0, 1.35);
    col += pGlow.rgb * exp(-dot(g, g) * 3.2) * 0.9;

    // Two terrains that the map slides between, back and forth.
    float morph = 0.5 + 0.5 * sin(time * 0.22);
    float2 q = p * 2.4;
    float h = lerp(Fbm(q + float2(time * 0.012, 0.0)), Fbm(q * 1.12 + float2(7.3, 2.1) - float2(0.0, time * 0.012)), morph);

    float v = (h - 0.5) * 30.0;                     // fbm clusters around 0.5, so stretch it
    float dist = abs(frac(v - 0.5) - 0.5) / max(fwidth(v), 1e-4); // pixels to the nearest contour
    float contour = 1.0 - smoothstep(0.35, 1.35, dist);
    float major = (fmod(floor(v + 0.5), 5.0) < 0.5) ? 1.0 : 0.4;

    // Lines glow a little brighter near the middle and fade toward the edges.
    float2 c = uv - 0.5;
    float centre = saturate(1.0 - dot(c, c) * 2.2);
    col += pAccent.rgb * contour * major * (0.05 + 0.17 * centre);
    col += pLight.rgb * smoothstep(0.55, 1.0, h) * 0.05; // faint haze over the "peaks"

    col *= 1.0 - dot(c, c) * 1.1;
    return float4(col, bgAlpha);
}

// ---- CRT: curvature, scanlines, aperture grille, bloom, power on/off ------

float4 PSCrt(VSOut i) : SV_Target
{
    float2 cc = i.uv * 2.0 - 1.0;
    cc *= 1.0 + (cc.yx * cc.yx) * curvature * 0.18;
    float2 uv = cc * 0.5 + 0.5;
    if (any(uv < 0.0) || any(uv > 1.0))
        return float4(0, 0, 0, 1);

    // Power on: a dot becomes a line, then the line opens into the picture.
    float sx = smoothstep(0.0, 0.35, power);
    float sy = lerp(0.006, 1.0, smoothstep(0.3, 1.0, power));
    float2 puv = (uv - 0.5) / float2(max(sx, 0.001), sy) + 0.5;
    if (any(puv < 0.0) || any(puv > 1.0))
        return float4(0, 0, 0, 1);
    float flash = 1.0 - smoothstep(0.25, 0.9, power);

    float2 px = 1.0 / resolution;
    float2 ca = (puv - 0.5) * px * 2.5 * (0.5 + curvature);
    float3 col;
    col.r = tex0.Sample(samp, puv + ca).r;
    col.g = tex0.Sample(samp, puv).g;
    col.b = tex0.Sample(samp, puv - ca).b;

    float3 bloom = 0;
    [unroll] for (int k = 0; k < 8; ++k)
    {
        float a = k * 0.7853982;
        float2 o = float2(cos(a), sin(a)) * px;
        bloom += tex0.Sample(samp, puv + o * 3.0).rgb + tex0.Sample(samp, puv + o * 7.0).rgb;
    }
    col += bloom / 16.0 * 0.3;

    float lines = clamp(resolution.y * 0.5, 240.0, 540.0);
    float s = 0.5 + 0.5 * cos(uv.y * lines * 6.2831853);
    col *= lerp(1.0, 0.45 + 0.55 * s, scanlines);

    float m = fmod(i.pos.x, 3.0);
    float3 mask = m < 1.0 ? float3(1.0, 0.75, 0.75) : (m < 2.0 ? float3(0.75, 1.0, 0.75) : float3(0.75, 0.75, 1.0));
    col *= lerp(float3(1, 1, 1), mask * 1.12, scanlines * 0.7);
    col *= 1.0 + scanlines * 0.25;

    float2 v = uv * (1.0 - uv);
    col *= saturate(pow(v.x * v.y * 16.0, 0.18 + 0.12 * curvature));

    col += (Hash21(i.pos.xy + frac(time * 7.13) * 431.0) - 0.5) * 0.035;
    col *= 0.985 + 0.015 * sin(time * 120.0);
    col += flash * float3(0.8, 0.9, 1.0);
    return float4(saturate(col), 1);
}

float4 PSCopy(VSOut i) : SV_Target
{
    return float4(tex0.Sample(samp, i.uv).rgb, 1);
}
)hlsl";
