/**
 * @file NoteVS.hlsl
 * @brief 描画テスト用 Vertex Shader
 * 全てのインスタンスに対して画面全体を覆う四角形を出力します。
 */

struct VS_INPUT {
    uint vertexID : SV_VertexID;
    uint instanceID : SV_InstanceID;
};

struct VS_OUTPUT {
    float4 position : SV_POSITION;
    float4 color : COLOR;
};

VS_OUTPUT main(VS_INPUT input) {
    VS_OUTPUT output;
    
    // 画面全体を覆う四角形 (-1.0 to 1.0)
    // 0: 左下, 1: 右下, 2: 左上, 3: 右上
    float2 pos[4] = {
        float2(-1.0f, -1.0f),
        float2( 1.0f, -1.0f),
        float2(-1.0f,  1.0f),
        float2( 1.0f,  1.0f)
    };

    output.position = float4(pos[input.vertexID % 4], 0.5f, 1.0f);
    output.color = float4(1.0f, 0.0f, 0.0f, 1.0f); // 強制的に赤色

    return output;
}
