#version 450
// Loops a push constant counts, one from 100 and one by 2 to the same count, a step of red and of green a pass:
// hardware loops whose passes the layer works out from the count, each loop's start and its step, as Shader Model 3
// counts them (tests/test_loops.c).
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { layout(offset = 32) vec4 step; int count; } pc;

void main()
{
	vec4 sum = vec4(0.0, 0.0, 0.0, 1.0);
	for (int i = 100; i < pc.count; i++)
		sum.r += pc.step.r;
	for (int j = 0; j < pc.count; j += 2)
		sum.g += pc.step.g;
	o_color = sum;
}
