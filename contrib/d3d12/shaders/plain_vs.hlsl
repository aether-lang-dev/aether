// Position only: what the pixel shader writes comes from elsewhere.
float4 main(float2 pos : TEXCOORD0) : SV_Position {
    return float4(pos, 0.0, 1.0);
}
