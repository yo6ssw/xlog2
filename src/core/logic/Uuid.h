#pragma once

#include <string>

// Toolkit-neutral UUID helpers used by logbook sync to give every QSO a stable
// cross-machine identity. No external dependency (a small SHA-1 lives in the
// .cpp), so the build's library set is unchanged.
namespace uuidutil {

// A fresh random version-4 UUID, e.g. "9f1c2e3a-...-...". Used for brand-new
// QSOs whose identity has no meaning beyond "distinct".
std::string newUuid();

// A deterministic version-5 (SHA-1, fixed namespace) UUID derived from
// `content`. The same content yields the same UUID on every machine, which is
// how two logbooks that started from a copy of the same file assign matching
// identities to the same pre-existing QSO during backfill.
std::string uuidV5(const std::string& content);

}  // namespace uuidutil
