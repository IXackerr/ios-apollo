#version 450
// vkdraw: a flat colour, so every covered pixel can be checked exactly.
layout(location = 0) out vec4 color;

void main()
{
	color = vec4(1.0, 0.5, 0.25, 1.0);
}
