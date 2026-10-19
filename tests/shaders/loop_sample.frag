#version 450
// Samples in the passes of a loop whose count is data, its eighth of the screen's, 3 to 297 passes: textureGrad at
// gradients the pass works out, the same across each 2x2 quad as the console takes one LOD a quad from register
// gradients (xsdk:docs/HARDWARE.md, "Samples in a loop's pass, milestone 46 step 6"), and the loop left on the level
// that sample took; textureLod at a LOD the pass and the pixel work out. Each pass's samples count half and what came
// before the other half; the counter's last three bits are blue (tests/test_loops.c).
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler2D u_levels;

void main()
{
	// gl_FragCoord sits on a pixel's centre, so a quarter of it, or a 160th, is never near a whole number.
	vec2 cell = floor(gl_FragCoord.xy - 4.0 * floor(gl_FragCoord.xy * 0.25));
	vec2 quad = floor(cell * 0.5);
	float band = floor(gl_FragCoord.x * 0.00625);
	vec2 seen = vec2(0.0);
	int i = 0;
	for (; i < int(band * band * 6.0 + 3.0); i++) {
		// The mip chain is 64 texels a side, so this LOD is the pass's number and the quad's column, to level 4.
		float gradient = exp2(min(float(i) + quad.x, 4.0) + 0.25) / 64.0;
		vec4 graded = textureGrad(u_levels, vec2(0.5), vec2(gradient, 0.0), vec2(0.0, gradient));
		// Each level's blue is its number in eighths.
		if (graded.b * 8.0 > band + quad.y - 0.5)
			break;
		float lod = mod(float(i) + cell.x + cell.y, 4.0) + 0.25;
		seen = seen * 0.5 + vec2(textureLod(u_levels, vec2(0.5), lod).r, graded.g) * 0.5;
	}
	o_color = vec4(seen, fract(float(i) * 0.125), 1.0);
}
