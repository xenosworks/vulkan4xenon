#version 450
// A loop whose count is data, in each eighth of the screen across from gl_FragCoord, 15 passes and 40 more an eighth,
// a step of colour a pass: a hardware loop of the most passes a loop constant runs, 255, which the layer sets, left on
// its condition (tests/test_loops.c).
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { layout(offset = 32) vec4 step; } pc;

void main()
{
	vec4 sum = vec4(0.0, 0.0, 0.0, 1.0);
	int count = int(gl_FragCoord.x * 0.00625) * 40 + 15;
	for (int i = 0; i < count; i++)
		sum.rgb += pc.step.rgb;
	o_color = sum;
}
