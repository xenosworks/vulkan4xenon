#version 450
// Breaks and continues on data in a loop of 8 passes, which xost unrolls: each pixel's column and row in a 4-pixel
// cell and its eighth of the screen pick the pass it continues past and the pass it breaks at, in either arm of an
// if/else on data, so the pixels of a quad leave at passes of their own (tests/test_loops.c).
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { layout(offset = 32) vec4 step; } pc;

void main()
{
	// gl_FragCoord sits on a pixel's centre, so a quarter of it, or a 160th, is never near a whole number.
	vec2 cell = floor(gl_FragCoord.xy - 4.0 * floor(gl_FragCoord.xy * 0.25));
	float band = floor(gl_FragCoord.x * 0.00625);
	vec4 sum = vec4(0.0, 0.0, 0.0, 1.0);
	for (int i = 0; i < 8; i++) {
		if (float(i) == cell.y + 2.0)
			continue;
		sum.r += pc.step.r;
		if (cell.x < 2.0) {
			sum.g += pc.step.g;
			if (float(i) >= band + cell.x)
				break;
		} else if (float(i) + cell.x >= band + 5.0) {
			break;
		}
		sum.b += float(i + 1) * pc.step.b;
	}
	o_color = sum;
}
