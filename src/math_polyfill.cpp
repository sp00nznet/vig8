// Polyfill for C23 math functions missing from MSVC CRT
// roundevenf: round to nearest even (banker's rounding)
// Used by SIMDE's SSE rounding intrinsics on MSVC targets.

#include <cmath>
#include <cstdint>

extern "C" float roundevenf(float v)
{
    float rounded = roundf(v);
    float diff = rounded - v;
    if (fabsf(diff) == 0.5f && (static_cast<int32_t>(rounded) & 1))
        rounded = v - diff;
    return rounded;
}
