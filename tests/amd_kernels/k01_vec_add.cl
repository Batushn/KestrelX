__kernel void k01_vec_add(__global const uint* a, __global const uint* b, __global uint* out) {
    uint gid = get_global_id(0);
    out[gid] = a[gid] + b[gid];
}
