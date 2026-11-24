__kernel void k11_mat4_mul(__global const float4* mat, __global const float4* vecs, __global float4* out) {
    uint gid = get_global_id(0);
    float4 v = vecs[gid];
    float4 r;
    r.x = dot(mat[0], v);
    r.y = dot(mat[1], v);
    r.z = dot(mat[2], v);
    r.w = dot(mat[3], v);
    out[gid] = r;
}
