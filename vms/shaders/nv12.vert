#version 440

layout(location = 0) in vec2 position;
layout(location = 1) in vec2 texcoord;

layout(location = 0) out vec2 v_texcoord;

layout(std140, binding = 0) uniform buf {
    mat4 mvp;
    mat4 colorMatrix;
} ubuf;

void main()
{
    v_texcoord = texcoord;
    gl_Position = ubuf.mvp * vec4(position, 0.0, 1.0);
}
