RWTexture2D<float4> InputOutput : register(u0);

cbuffer ColorGradeParams : register(b0) {
    float exposure;
    float contrast;
    float temperature;
    float tint;
    float saturation;
    float _pad[3];  // 16-byte alignment
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    // bounds check
    uint2 dims; InputOutput.GetDimensions(dims.x, dims.y);
    if (id.x >= dims.x || id.y >= dims.y) return;

    float4 pixel = InputOutput[id.xy];

    // --- apply 5 ops in sequence ---
    // exposure
    pixel.rgb *= pow(2.0, exposure);

    // contrast
    pixel.rgb = (pixel.rgb - 0.5) * contrast + 0.5;

    // temperature
    pixel.r += temperature * 0.1;
    pixel.b -= temperature * 0.1;

    // tint
    pixel.g += tint * 0.1;

    // saturation
    float luma = dot(pixel.rgb, float3(0.2126, 0.7152, 0.0722));
    pixel.rgb = lerp(luma, pixel.rgb, saturation);

    // clamp with saturate()
    pixel.rgb = saturate(pixel.rgb);

    InputOutput[id.xy] = pixel;
}