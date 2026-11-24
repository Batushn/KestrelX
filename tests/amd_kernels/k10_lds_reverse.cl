__kernel void k10_lds_reverse(__global const uint* inp, __global uint* out) {
    __local uint tile[32];
    uint lid = get_local_id(0);
    uint gid = get_global_id(0);
    tile[lid] = inp[gid] ^ (lid * 0x9E3779B9u);
    barrier(CLK_LOCAL_MEM_FENCE);
    out[gid] = tile[31u - lid] + lid;
}
