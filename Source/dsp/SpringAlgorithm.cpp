#include "Algorithms.h"

namespace rivage
{

//==============================================================================
// PURE: the linear waveguide exactly as it has always been. Everything lives in
// the shared SpringCore template; this file only forwards.

void SpringAlgorithm::prepare (double newSampleRate)
{
    core.prepare (newSampleRate);
}

void SpringAlgorithm::reset()
{
    core.reset();
}

void SpringAlgorithm::setParams (const AlgorithmParams& newParams)
{
    core.setParams (newParams);
}

void SpringAlgorithm::snap()
{
    core.snap();
}

void SpringAlgorithm::processSample (float inL, float inR, float& outL, float& outR)
{
    core.processSample (inL, inR, outL, outR);
}

} // namespace rivage
