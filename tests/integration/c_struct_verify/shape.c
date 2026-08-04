#include "shape.h"

static shape g_shape = { 0, 4242, 77 };

shape* shape_new(void) { return &g_shape; }
