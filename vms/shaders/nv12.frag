#version 440

// NV12 -> RGB 변환을 GPU에서 수행한다.
// CPU videoconvert(색공간 변환) + QPainter 스케일링을 통째로 제거하는 것이
// 이 셰이더의 존재 이유다. 디코더가 내는 NV12를 그대로 업로드해 샘플링한다.

layout(location = 0) in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 mvp;
    mat4 colorMatrix;  // vec4(y, u, v, 1) -> rgb (계수+오프셋 포함)
} ubuf;

layout(binding = 1) uniform sampler2D texY;   // R8:  루마
layout(binding = 2) uniform sampler2D texUV;  // RG8: 크로마 (1/2 해상도)

void main()
{
    float y = texture(texY, v_texcoord).r;
    vec2 uv = texture(texUV, v_texcoord).rg;
    vec3 rgb = (ubuf.colorMatrix * vec4(y, uv.x, uv.y, 1.0)).rgb;
    fragColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
