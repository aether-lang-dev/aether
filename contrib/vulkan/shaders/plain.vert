#version 450

// Position only: what the fragment shader writes comes from elsewhere.
layout(location = 0) in vec2 inPosition;

void main() {
    gl_Position = vec4(inPosition, 0.0, 1.0);
}
