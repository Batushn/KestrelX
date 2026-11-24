__kernel void k08_bitfield(__global const uint* inp, __global uint* out, uint shift, uint mask) {
    uint gid = get_global_id(0);
    uint v = inp[gid];
    uint lo = (v >> shift) & mask;
    uint hi = ((v & mask) << (32u - shift)) | (v >> shift);
    out[gid] = (lo & 0xFFFFu) | ((hi & 0xFFFFu) << 16);
}
