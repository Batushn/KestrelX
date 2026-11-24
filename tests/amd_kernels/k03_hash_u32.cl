__kernel void k03_hash_u32(__global const uint* inp, __global uint* out, uint seed) {
    uint gid = get_global_id(0);
    uint x = inp[gid] ^ seed;
    x = (x ^ 61u) ^ (x >> 16);
    x = x + (x << 3);
    x = x ^ (x >> 4);
    x = x * 0x27d4eb2du;
    x = x ^ (x >> 15);
    out[gid] = x;
}
