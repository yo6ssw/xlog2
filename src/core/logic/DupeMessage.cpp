#include "DupeMessage.h"

namespace dupe {

std::string format(const Qso& e) {
    return "⚠ Dupe — already worked " + e.call + " on " + e.band + " " + e.mode +
           " at " + e.time_on + " (" + e.date + ")";
}

}  // namespace dupe
