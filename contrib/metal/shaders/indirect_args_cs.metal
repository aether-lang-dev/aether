// Writes `n` 32-bit words from word `at` of a buffer: the commands an
// indirect draw reads, set on the GPU (#2198). Run with one thread a group.
#include <metal_stdlib>
using namespace metal;

struct Push {
    uint at;
    uint n;
    uint v[5];
};

kernel void indirect_args(device uint* words [[buffer(0)]],
                          constant Push& push [[buffer(8)]],
                          uint i [[thread_position_in_grid]]) {
    if (i != 0) return;
    for (uint k = 0; k < push.n; k++) {
        words[push.at + k] = push.v[k];
    }
}
