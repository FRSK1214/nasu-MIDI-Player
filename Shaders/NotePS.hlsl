struct PS_INPUT {
    float4 position : SV_POSITION;
    float4 color : COLOR;
};

float4 main(PS_INPUT input) : SV_TARGET {
    return float4(1.0f, 0.0f, 0.0f, 1.0f); // 真っ赤に出力
}
