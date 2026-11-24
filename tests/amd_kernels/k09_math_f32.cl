__kernel void k09_math_f32(__global const float* inp, __global float* out) {
    uint gid = get_global_id(0);
    float x = fabs(inp[gid]) + 1.0f;
    float s = native_sqrt(x);
    float r = native_rsqrt(x);
    out[gid] = floor(s) + native_recip(x) + r;
}
