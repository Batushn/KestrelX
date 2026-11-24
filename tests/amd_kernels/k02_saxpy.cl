__kernel void k02_saxpy(__global const float* x, __global const float* y, __global float* out, float alpha) {
    uint gid = get_global_id(0);
    out[gid] = fma(alpha, x[gid], y[gid]);
}
