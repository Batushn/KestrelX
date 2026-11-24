__kernel void k07_loop_collatz(__global const uint* inp, __global uint* out, uint max_steps) {
    uint gid = get_global_id(0);
    uint n = inp[gid];
    uint steps = 0u;
    for (uint i = 0u; i < max_steps; ++i) {
        if (n <= 1u) break;
        n = (n & 1u) ? (3u * n + 1u) : (n >> 1);
        ++steps;
    }
    out[gid] = steps ^ (n << 16);
}
