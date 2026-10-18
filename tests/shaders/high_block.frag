#version 450
// A uniform block of 253 rows, its last at c252, in the group xsdk keeps for gl_FragCoord: the layer refuses the
// pipeline (tests/test_tiles.c).
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform Rows { vec4 rows[253]; } u;

void main()
{
	o_color = u.rows[252];
}
