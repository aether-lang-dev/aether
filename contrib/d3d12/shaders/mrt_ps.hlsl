// Three outputs, one an attachment (#2386): the interpolated colour, a fixed
// colour, and an HDR one a float attachment keeps whole.
struct PSOut {
    float4 c0 : SV_Target0;
    float4 c1 : SV_Target1;
    float4 c2 : SV_Target2;
};

PSOut main(float4 pos : SV_Position, float3 col : COLOR0) {
    PSOut o;
    o.c0 = float4(col, 1.0);
    o.c1 = float4(0.25, 0.5, 0.75, 1.0);
    o.c2 = float4(2.0, -1.0, 0.5, 1.0);
    return o;
}
