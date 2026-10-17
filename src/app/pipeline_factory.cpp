// INTEGRATOR: replace this file to return the real pipeline (VDD -> DXGI capture -> GPU -> MS912x).
// Everything else in src/app only knows IPipeline (core/pipeline.h).
#include "core/pipeline.h"

namespace msdisp {

std::unique_ptr<IPipeline> createPipeline() { return createFakePipeline(); }

}  // namespace msdisp
