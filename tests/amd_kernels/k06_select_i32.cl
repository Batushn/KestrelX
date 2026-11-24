__kernel void k06_select_i32(__global const int* a, __global const int* b, __global int* out, int thresh) {
    uint gid = get_global_id(0);
    int va = a[gid];
    int vb = b[gid];
    int m = (va > vb) ? va : vb;
    out[gid] = (m >= thresh) ? (m - thresh) : (-m);
}
