// Explicit instantiation: forces every member of the engine through the compiler and the warning
// set, whichever of them a given tool happens to use. No allocation, exceptions or I/O in
// on_message.
#include "llme/engine.hpp"

namespace llme {

template class MatchingEngine<LadderLevelStore, NullSink>;

}  // namespace llme
