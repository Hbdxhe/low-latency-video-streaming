#version 330 core

layout (location = 0) in vec2 a_position;
layout (location = 1) in vec2 a_tex_coord;

uniform vec2 u_scale;

out vec2 v_tex_coord;

void main() {
    gl_Position = vec4(a_position * u_scale, 0.0, 1.0);
    v_tex_coord = a_tex_coord;
}
