// Explicit instantiation: forces every member of the engine through the compiler and the warning
// set, whichever of them a given tool happens to use. No allocation, exceptions or I/O in
// on_message.
#include "llme/engine.hpp"
#include "llme/level_store_map.hpp"
#include "llme/level_store_sorted_vec.hpp"

namespace llme {

template class MatchingEngine<LadderLevelStore, NullSink>;
template class MatchingEngine<MapLevelStore, NullSink>;
template class MatchingEngine<SortedVecLevelStore, NullSink>;

}  // namespace llme
