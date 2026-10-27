#version 450
// A quad from gl_VertexIndex alone, its corner as PPSSPP's stencil upload works one out, in the cell its index's quad
// picks: corner (i & 1, (i & 2) / 2) of cell i >> 2, the push constants placing cell 0, the step to the next and the
// quad's size (tests/test_vertex_index.c).
layout(push_constant) uniform Push { vec4 origin; vec4 step; } pc;

void main()
{
	vec2 corner = vec2(float((gl_VertexIndex & 1) << 1), float(gl_VertexIndex & 2)) * 0.5;
	vec2 cell = vec2(float(gl_VertexIndex >> 2), 0.0);
	gl_Position = vec4(pc.origin.xy + cell * pc.step.xy + corner * pc.step.zw, 0.0, 1.0);
}
