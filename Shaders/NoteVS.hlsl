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

    float2 pos[4] = {
        float2(-1.0f, -1.0f),
        float2( 1.0f, -1.0f),
        float2(-1.0f,  1.0f),
        float2( 1.0f,  1.0f)
    };

    output.position = float4(pos[input.vertexID % 4], 0.5f, 1.0f);
    output.color = float4(1.0f, 0.0f, 0.0f, 1.0f);

    return output;
}
