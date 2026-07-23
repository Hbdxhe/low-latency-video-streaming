#version 330 core

in vec2 v_tex_coord;

uniform sampler2D u_plane0;
uniform sampler2D u_plane1;
uniform sampler2D u_plane2;
uniform int u_color_mode;

out vec4 frag_color;

vec3 yuv_to_rgb(float y, float u, float v) {
    float u_shifted = u - 0.5;
    float v_shifted = v - 0.5;
    return vec3(
        y + 1.402 * v_shifted,
        y - 0.344136 * u_shifted - 0.714136 * v_shifted,
        y + 1.772 * u_shifted
    );
}

void main() {
    if (u_color_mode == 1) {
        float y = texture(u_plane0, v_tex_coord).r;
        float u = texture(u_plane1, v_tex_coord).r;
        float v = texture(u_plane2, v_tex_coord).r;
        frag_color = vec4(yuv_to_rgb(y, u, v), 1.0);
        return;
    }

    if (u_color_mode == 2) {
        float y = texture(u_plane0, v_tex_coord).r;
        vec2 uv = texture(u_plane1, v_tex_coord).rg;
        frag_color = vec4(yuv_to_rgb(y, uv.x, uv.y), 1.0);
        return;
    }

    frag_color = vec4(texture(u_plane0, v_tex_coord).rgb, 1.0);
}
