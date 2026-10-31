#version 430
// Post-processing: bloom (from the texture's mipmaps), exposure, tone mapping
in vec2 vUV;
out vec4 fragColor;

uniform sampler2D uImage;
uniform int   uBloom;
uniform float uExposure;

vec3 aces(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// ACES on each channel pushes bright colours towards white, because all three channels run
// into the top of the curve. So the curve goes on the brightest channel alone, and the other
// two keep their ratio to it, which keeps the colour of the gas. Only the hottest light, far
// up the curve, is allowed to go to white.
vec3 toneMap(vec3 x) {
    float peak = max(x.r, max(x.g, x.b));
    vec3 keepColour = x / max(peak, 1e-5) * aces(vec3(peak));
    return mix(keepColour, aces(x), mix(0.35, 1.0, smoothstep(2.5, 7.0, peak)));
}

// One blurred version of the image. Four lookups around the pixel smooth out the blocky
// grid of the small mip levels, which otherwise shows as stairs around very bright areas.
vec3 blurred(float lod) {
    vec2 t = 0.5 * pow(2.0, lod) / vec2(textureSize(uImage, 0));
    return 0.25 * (textureLod(uImage, vUV + vec2(-t.x, -t.y), lod).rgb
                 + textureLod(uImage, vUV + vec2( t.x, -t.y), lod).rgb
                 + textureLod(uImage, vUV + vec2(-t.x,  t.y), lod).rgb
                 + textureLod(uImage, vUV + vec2( t.x,  t.y), lod).rgb);
}

void main() {
    vec3 col = texture(uImage, vUV).rgb;

    if (uBloom == 1) {
        // Blurry versions of the image are already in the mip chain.
        // Keep only the bright parts and add them back as glow.
        vec3 glow = vec3(0);
        glow += blurred(2.0) * 0.30;
        glow += blurred(3.0) * 0.25;
        glow += blurred(4.0) * 0.20;
        glow += blurred(5.0) * 0.15;
        glow += blurred(6.0) * 0.10;
        glow = max(glow - 0.15, 0.0);
        col += glow / (1.0 + 0.25 * glow) * 0.8;    // a soft limit, so a hot core can't fog the frame
    }


    col = toneMap(col * uExposure);
    col = pow(col, vec3(1.0 / 2.2));   // gamma
    fragColor = vec4(col, 1.0);
}
