#include "common.hlsli"

// Oscilloscope phosphor decay. The output is ignored: the blend state multiplies what's
// already in the buffer by a per-channel blend factor (slow afterglow, fast core).
float4 main(VSOut i) : SV_Target {
    return 0;
}
