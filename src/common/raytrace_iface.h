// Ray-Trace Metamod module interface

#pragma once

#include <cstdint>

#define RAYTRACE_INTERFACE_VERSION "CRayTraceInterface002"

namespace cs2bv::rt {
// Source Vector
struct Vector
{
    float x, y, z;
};

struct TraceOptions
{
    uint64_t interactsWith = 0x2c3011; // MASK_SHOT_PHYSICS
    uint64_t interactsExclude = 0;
    int drawBeam = 0;
};

struct TraceResult
{
    Vector endPos{};
    void* hitEntity = nullptr;
    float fraction = 1.0f;
    int allSolid = 0;
    Vector normal{};
};

class CRayTraceInterface
{
  public:
    // Releases an external ray-trace interface
    virtual ~CRayTraceInterface() = default;

    // Traces from a point using an angle representation
    virtual bool
    TraceShape(const Vector* start, const void* angles, void* ignoreEntity, TraceOptions* traceOptions, TraceResult* traceResult) = 0;

    // Traces between two explicit positions
    virtual bool
    TraceEndShape(const Vector* start, const Vector* end, void* ignoreEntity, TraceOptions* traceOptions, TraceResult* traceResult) = 0;
};
} // namespace cs2bv::rt
