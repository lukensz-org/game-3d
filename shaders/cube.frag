#version 450

layout(location = 0) flat in vec4 v_color;
layout(location = 0) out vec4 output_color;

void main() {
    output_color = v_color;
}
