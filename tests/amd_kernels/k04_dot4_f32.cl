__kernel void k04_dot4_f32(__global const float4* a, __global const float4* b, __global float* out) {
    uint gid = get_global_id(0);
    float4 va = a[gid];
    float4 vb = b[gid];
    out[gid] = va.x * vb.x + va.y * vb.y + va.z * vb.z + va.w * vb.w;
}
