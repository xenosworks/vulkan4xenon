#version 450
// Samples the image being drawn back into eDRAM, for each of four colour targets, as a pass past eDRAM can't set one
// alone: the write masks keep all but the image's own.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color0;
layout(location = 1) out vec4 o_color1;
layout(location = 2) out vec4 o_color2;
layout(location = 3) out vec4 o_color3;
layout(set = 0, binding = 0) uniform sampler2D u_image;

void main()
{
	vec4 color = texture(u_image, v_uv);
	o_color0 = color;
	o_color1 = color;
	o_color2 = color;
	o_color3 = color;
}
