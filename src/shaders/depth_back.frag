#version 450
// Draws a depth image's depth back into eDRAM, as it samples in red, for a pass past eDRAM that loads depth stored a
// band at a time; the write masks keep every colour target.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler2D u_image;

void main()
{
	o_color = vec4(0.0);
	gl_FragDepth = texture(u_image, v_uv).r;
}
