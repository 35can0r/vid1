@group(0) @binding(0) var src1: texture_2d<f32>;
@group(0) @binding(1) var src2: texture_2d<f32>;
@group(0) @binding(2) var dest: texture_storage_2d<rgba8unorm, write>;

@compute
@workgroup_size(16, 16, 1)
fn main(@builtin(global_invocation_id) global_id: vec3<u32>) {
    let dim = textureDimensions(src1);

    if (global_id.x >= dim.x || global_id.y >= dim.y) {
        return;
    }

    let coords = vec2<i32>(i32(global_id.x), i32(global_id.y));

    // Sample the two textures
    let color1 = textureLoad(src1, coords, 0);
    let color2 = textureLoad(src2, coords, 0);

    // Simple alpha blending: out = src2 * src2.a + src1 * (1 - src2.a)
    // Assume src2 is on top of src1.
    let alpha = color2.a;
    let blended_rgb = color2.rgb * alpha + color1.rgb * (1.0 - alpha);
    let out_color = vec4<f32>(blended_rgb, max(color1.a, color2.a));

    textureStore(dest, coords, out_color);
}
