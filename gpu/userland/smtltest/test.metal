// smtltest shaders: a full-screen triangle with a flat colour, and a kernel
// that squares a buffer; both checked by smtltest.c.
#include <metal_stdlib>
using namespace metal;

struct VOut {
	float4 position [[position]];
};

vertex VOut tri_vertex(uint vid [[vertex_id]])
{
	const float2 pos[3] = { float2(-0.8, 0.8), float2(0.8, 0.8), float2(0.0, -0.8) };
	VOut o;
	o.position = float4(pos[vid], 0.0, 1.0);
	return o;
}

fragment float4 tri_fragment(VOut in [[stage_in]], constant float4 &color [[buffer(0)]])
{
	return color;
}

kernel void square(device uint *data [[buffer(0)]], uint i [[thread_position_in_grid]])
{
	uint x = data[i];
	data[i] = x * x + (i ^ 0x9e3779b9u);
}
