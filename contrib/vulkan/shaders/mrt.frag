#version 450

// Three outputs, one an attachment (#2386): the interpolated colour, a fixed
// colour, and an HDR one a float attachment keeps whole.
layout(location = 0) in vec3 fragColor;

layout(location = 0) out vec4 out0;
layout(location = 1) out vec4 out1;
layout(location = 2) out vec4 out2;

void main() {
    out0 = vec4(fragColor, 1.0);
    out1 = vec4(0.25, 0.5, 0.75, 1.0);
    out2 = vec4(2.0, -1.0, 0.5, 1.0);
}
