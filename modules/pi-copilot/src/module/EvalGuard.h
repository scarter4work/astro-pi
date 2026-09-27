// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_EvalGuard_h
#define PICopilot_EvalGuard_h

#include <atomic>

namespace pcl
{

// Process-wide EvaluateScript depth (controller ruling, measured in plan
// Task 1): an EvaluateScript nested inside another one -- e.g. from a Timer
// tick while the outer script pumps events -- REPLACES the outer call's return
// value, silently. So every module EvaluateScript caller holds an
// EvalDepthGuard around the call, and a caller that may run from a timer tick
// (HistoryReader, the journey tracker) checks EvaluateScriptDepth() first and
// defers to the next tick while it is > 0 -- never blocks, never nests.
// Root thread only, like EvaluateScript itself; atomic only for cheapness of
// a correct read.
inline std::atomic<int>& EvalDepthCounter()
{
   static std::atomic<int> depth( 0 );
   return depth;
}

inline int EvaluateScriptDepth()
{
   return EvalDepthCounter().load();
}

class EvalDepthGuard
{
public:

   EvalDepthGuard()  { ++EvalDepthCounter(); }
   ~EvalDepthGuard() { --EvalDepthCounter(); }

   EvalDepthGuard( const EvalDepthGuard& ) = delete;
   EvalDepthGuard& operator =( const EvalDepthGuard& ) = delete;
};

} // namespace pcl

#endif // PICopilot_EvalGuard_h
