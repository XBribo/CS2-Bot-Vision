// Ray-Trace Metamod module interface

#pragma once

#include <cstdint>

#define RAYTRACE_INTERFACE_VERSION "CRayTraceInterface002"

namespace cs2bv::rt
{
    // Source Vector
    struct Vector
    {
        float x, y, z;
    };

    struct TraceOptions
    {
        uint64_t InteractsWith = 0x2c3011; // MASK_SHOT_PHYSICS
        uint64_t InteractsExclude = 0;
        int DrawBeam = 0;
    };

    struct TraceResult
    {
        Vector EndPos{};
        void *HitEntity = nullptr;
        float Fraction = 1.0f;
        int AllSolid = 0;
        Vector Normal{};
    };

    class CRayTraceInterface
    {
    public:
        // Releases an external ray-trace interface
        virtual ~CRayTraceInterface() = default;

        // Traces from a point using an angle representation
        virtual bool TraceShape(const Vector *pVecStart, const void *pAngAngles,
                                void *pIgnoreEntity, TraceOptions *pTraceOptions,
                                TraceResult *pTraceResult) = 0;

        // Traces between two explicit positions
        virtual bool TraceEndShape(const Vector *pVecStart, const Vector *pVecEnd,
                                   void *pIgnoreEntity, TraceOptions *pTraceOptions,
                                   TraceResult *pTraceResult) = 0;
    };
}
