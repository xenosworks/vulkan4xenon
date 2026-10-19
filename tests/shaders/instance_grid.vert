#version 450
// A quad an instance in a grid of 40 by 25 cells over the screen (milestone 47): its cell from gl_InstanceIndex, its
// colour the instance's own attribute.
layout(location = 0) in vec2 a_corner;
layout(location = 1) in vec4 a_color;
layout(location = 0) out vec4 v_color;

void main()
{
	float index = float(gl_InstanceIndex);
	float row = floor((index + 0.5) / 40.0);
	vec2 cell = vec2(index - row * 40.0, row);
	gl_Position = vec4((cell + vec2(0.2) + a_corner * 0.6) * vec2(2.0 / 40.0, 2.0 / 25.0) - vec2(1.0), 0.0, 1.0);
	v_color = a_color;
}
