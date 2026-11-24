__kernel void k12_morton_2d(__global const uint* xs, __global const uint* ys, __global uint* out) {
    uint gid = get_global_id(0);
    uint x = xs[gid] & 0xFFFFu;
    uint y = ys[gid] & 0xFFFFu;
    x = (x | (x << 8)) & 0x00FF00FFu;
    x = (x | (x << 4)) & 0x0F0F0F0Fu;
    x = (x | (x << 2)) & 0x33333333u;
    x = (x | (x << 1)) & 0x55555555u;
    y = (y | (y << 8)) & 0x00FF00FFu;
    y = (y | (y << 4)) & 0x0F0F0F0Fu;
    y = (y | (y << 2)) & 0x33333333u;
    y = (y | (y << 1)) & 0x55555555u;
    out[gid] = x | (y << 1);
}
