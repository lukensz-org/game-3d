#version 450

layout(push_constant) uniform Cube {
    mat4 mvp;
    vec4 color;
} cube;

layout(location = 0) flat out vec4 v_color;

// Unit cube corners in [-0.5, 0.5], two triangles per face.
const vec3 positions[36] = vec3[](
    // -Z
    vec3(-0.5, -0.5, -0.5), vec3( 0.5, -0.5, -0.5), vec3( 0.5,  0.5, -0.5),
    vec3(-0.5, -0.5, -0.5), vec3( 0.5,  0.5, -0.5), vec3(-0.5,  0.5, -0.5),
    // +Z
    vec3(-0.5, -0.5,  0.5), vec3( 0.5,  0.5,  0.5), vec3( 0.5, -0.5,  0.5),
    vec3(-0.5, -0.5,  0.5), vec3(-0.5,  0.5,  0.5), vec3( 0.5,  0.5,  0.5),
    // -X
    vec3(-0.5, -0.5, -0.5), vec3(-0.5,  0.5, -0.5), vec3(-0.5,  0.5,  0.5),
    vec3(-0.5, -0.5, -0.5), vec3(-0.5,  0.5,  0.5), vec3(-0.5, -0.5,  0.5),
    // +X
    vec3( 0.5, -0.5, -0.5), vec3( 0.5, -0.5,  0.5), vec3( 0.5,  0.5,  0.5),
    vec3( 0.5, -0.5, -0.5), vec3( 0.5,  0.5,  0.5), vec3( 0.5,  0.5, -0.5),
    // -Y
    vec3(-0.5, -0.5, -0.5), vec3(-0.5, -0.5,  0.5), vec3( 0.5, -0.5,  0.5),
    vec3(-0.5, -0.5, -0.5), vec3( 0.5, -0.5,  0.5), vec3( 0.5, -0.5, -0.5),
    // +Y
    vec3(-0.5,  0.5, -0.5), vec3( 0.5,  0.5, -0.5), vec3( 0.5,  0.5,  0.5),
    vec3(-0.5,  0.5, -0.5), vec3( 0.5,  0.5,  0.5), vec3(-0.5,  0.5,  0.5)
);

void main() {
    gl_Position = cube.mvp * vec4(positions[gl_VertexIndex], 1.0);
    v_color = cube.color;
}
