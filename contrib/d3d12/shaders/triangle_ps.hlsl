float4 main(float4 pos : SV_Position, float3 col : COLOR0) : SV_Target {
    return float4(col, 1.0);
}
