// "fake.subdivide": a deterministic test backend that is always built. It gives the worker protocol, the GUI and
// the parity harness a real end-to-end path before (and independent of) the Quadremesh intake.
#pragma once

#include <memory>

#include "rdesk/backend.hpp"

namespace rdesk::backends {

std::shared_ptr<IRemeshBackend> make_fake_backend();

// Registers "fake.subdivide" in the global registry (replaces an existing one).
void register_fake_backend();

}  // namespace rdesk::backends
