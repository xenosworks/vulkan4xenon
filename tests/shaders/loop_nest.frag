#version 450
// Hardware loops nested 3 deep: a loop a push constant counts, around a while on data, whose pass holds an if/else on
// data with a loop the push constant counts, left on data, in one arm and a do/while on data in the other. Each
// pixel's column and row in a 4-pixel cell and its eighth of the screen give it passes of its own at every level; what
// each level ran is the colour (tests/test_loops.c).
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { layout(offset = 32) vec4 step; int count; } pc;

void main()
{
	// gl_FragCoord sits on a pixel's centre, so a quarter of it, or a 160th, is never near a whole number.
	vec2 cell = floor(gl_FragCoord.xy - 4.0 * floor(gl_FragCoord.xy * 0.25));
	float band = floor(gl_FragCoord.x * 0.00625);
	float a = 0.0;
	float b = 0.0;
	float c = 0.0;
	for (int i = 0; i < pc.count; i++) {
		float t = 0.0;
		while (t < band * 0.5 + cell.x - float(i)) {
			t += 1.0;
			if (cell.y < 2.0) {
				for (int j = 0; j < pc.count; j++) {
					if (float(j) >= t + cell.y)
						break;
					c += 1.0;
				}
			} else {
				float k = 0.0;
				do {
					k += 1.0;
					b += 1.0;
				} while (k < t - cell.y + 2.0);
			}
		}
		a += t;
	}
	o_color = vec4(a * pc.step.r, b * pc.step.g, c * pc.step.b, 1.0);
}
