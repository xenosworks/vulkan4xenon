#version 450
// Breaks and continues on data in a hardware loop a push constant counts: each pixel's column and row in a 4-pixel
// cell and its eighth of the screen pick the pass it continues past, the pass it breaks at in an arm of an if/else on
// data and the pass whose end it breaks at, so the pixels of a quad, and of each group the GPU runs together, leave at
// passes of their own; the counter after the loop is blue (tests/test_loops.c).
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { layout(offset = 32) vec4 step; int count; } pc;

void main()
{
	// gl_FragCoord sits on a pixel's centre, so a quarter of it, or a 160th, is never near a whole number.
	vec2 cell = floor(gl_FragCoord.xy - 4.0 * floor(gl_FragCoord.xy * 0.25));
	float band = floor(gl_FragCoord.x * 0.00625);
	vec2 sum = vec2(0.0);
	int i = 0;
	for (; i < pc.count; i++) {
		if (float(i) == cell.y * 2.0 + 1.0)
			continue;
		sum.x += pc.step.r;
		if (cell.x < 2.0) {
			sum.y += pc.step.g;
			if (float(i) >= band * 2.0 + cell.x)
				break;
		}
		if (float(i) + cell.x >= band * 4.0 + 5.0)
			break;
	}
	o_color = vec4(sum, float(i) * pc.step.b, 1.0);
}
