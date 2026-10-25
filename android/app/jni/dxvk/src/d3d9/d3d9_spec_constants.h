#pragma once

#include <cstdint>

namespace dxvk {

  enum D3D9SpecConstantId : uint32_t {
    AlphaCompareOp  = 0,
    SamplerType     = 1,
    FogEnabled      = 2,
    VertexFogMode   = 3,
    PixelFogMode    = 4,

    PointMode       = 5,
    ProjectionType  = 6,

    VertexShaderBools = 7,
    PixelShaderBools  = 8,
    Fetch4            = 9,

    SamplerDepthMode  = 10,

    // Port: per vertex-shader input, bit n = x255 (UBYTE4 fetched as UNORM),
    // bit 16+n = x32767 (SHORT2/4 fetched as SNORM), for GPUs without SCALED vertex formats.
    VertexScaleMask   = 11,
  };

}