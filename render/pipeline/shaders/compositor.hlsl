// Per-layer constant buffer (updated between draws)
cbuffer LayerCB : register(b0) {
    float2 quad_center;    // canvas NDC center: center_x*2-1, -(center_y*2-1)
    float2 quad_half_size; // NDC half-size: width, height
    float  rotation;       // radians
    float  opacity;
    float2 uv_min;         // after crop: (crop.left, crop.top)
    float2 uv_max;         // after crop: (1-crop.right, 1-crop.bottom)
    // Color grade params
    float  exposure;
    float  contrast;
    float  temperature;
    float  tint;
    float  saturation;
    float3 _pad;
};

Texture2D<float4>   SourceTex : register(t0);
SamplerState        Sampler   : register(s0);  // bilinear, clamp

struct VSInput  { float2 pos : POSITION; float2 uv : TEXCOORD0; };
struct PSInput  { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

PSInput VSMain(VSInput vin) {
    // Unit quad: pos in [-0.5, 0.5], uv in [0, 1]
    // 1. Scale to quad size in NDC
    float2 p = vin.pos * quad_half_size * 2.0;
    // 2. Rotate around origin
    float  s = sin(rotation), c = cos(rotation);
    float2 r = float2(p.x*c - p.y*s, p.x*s + p.y*c);
    // 3. Translate to canvas position (NDC: Y flipped)
    PSInput o;
    o.pos = float4(r + quad_center, 0, 1);
    // UV: map [0,1] quad UV to cropped source UV
    o.uv  = uv_min + vin.uv * (uv_max - uv_min);
    return o;
}

float4 PSMain(PSInput pin) : SV_Target {
    float4 pixel = SourceTex.Sample(Sampler, pin.uv);

    // Step 1: Exposure
    float E = pow(2.0, exposure);
    pixel.rgb *= E;

    // Step 2: Contrast
    pixel.rgb = (pixel.rgb - 0.5) * contrast + 0.5;

    // Step 3: Temperature & Tint
    pixel.r += temperature * 0.1;
    pixel.g += tint        * 0.1;
    pixel.b -= temperature * 0.1;

    // Step 4: Saturation (REC.709 luma)
    float luma = dot(pixel.rgb, float3(0.2126, 0.7152, 0.0722));
    pixel.rgb = lerp(luma, pixel.rgb, saturation);

    // Step 5: Clamp + apply opacity
    pixel.rgb = saturate(pixel.rgb);
    pixel.a  *= opacity;

    return pixel;
}
