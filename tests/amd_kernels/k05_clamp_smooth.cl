__kernel void k05_clamp_smooth(__global const float* inp, __global float* out, float lo, float hi) {
    uint gid = get_global_id(0);
    float t = clamp((inp[gid] - lo) / (hi - lo), 0.0f, 1.0f);
    out[gid] = t * t * (3.0f - 2.0f * t);
}
