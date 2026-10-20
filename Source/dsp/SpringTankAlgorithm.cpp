#include "Algorithms.h"

namespace rivage
{

//==============================================================================
// TANK: the same waveguide with the physical chain around it. The drive
// transducer, the level dependent damper, the impact layer and the pickup
// saturation all live in SpringCore's kTank branch.

void SpringTankAlgorithm::prepare (double newSampleRate)
{
    core.prepare (newSampleRate);
}

void SpringTankAlgorithm::reset()
{
    core.reset();
}

void SpringTankAlgorithm::setParams (const AlgorithmParams& newParams)
{
    core.setParams (newParams);
}

void SpringTankAlgorithm::snap()
{
    core.snap();
}

void SpringTankAlgorithm::processSample (float inL, float inR, float& outL, float& outR)
{
    core.processSample (inL, inR, outL, outR);
}

} // namespace rivage
