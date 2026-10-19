#version 450
// mesh.vert's cube placed by its instance's matrix, and tinted by its instance's tint for sprite.frag, both at the
// instance's rate.
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_mvp0;
layout(location = 3) in vec4 a_mvp1;
layout(location = 4) in vec4 a_mvp2;
layout(location = 5) in vec4 a_mvp3;
layout(location = 6) in vec4 a_tint;
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;

void main()
{
	// The matrix times vec4(a_position, 1.0), column by column from the first, as xost multiplies mesh.vert's
	// (xsdk:src/xsc_lower_matrix.c, vec_mat_ops); xost builds no matrix from inputs.
	gl_Position = a_mvp0 * a_position.x + a_mvp1 * a_position.y + a_mvp2 * a_position.z + a_mvp3;
	v_uv = a_uv;
	v_color = a_tint;
}
