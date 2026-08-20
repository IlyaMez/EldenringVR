#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ervr {
// Finds the primary virtual function table of an MSVC RTTI-enabled class in the
// main module by its demangled type-descriptor name, e.g. "CS::NoUpdateInterface"
// (matched against ".?AVNoUpdateInterface@CS@@"). Returns nullptr if not found.
void** FindVTable(const char* className);

// Every RTTI class in the main module whose demangled name contains `substr`. Lets us
// discover what a class is really called on this build instead of guessing names.
std::vector<std::string> FindTypeNames(const char* substr);

// Singleton hunting: scans the main module's non-executable sections for pointer slots that
// point at an object whose first field is `vt`. Dantelion2 keeps its managers in statics, so
// this finds them in a few megabytes instead of scanning the game's multi-gigabyte heap -
// which page-faults so hard it evicts the game's own working set.
std::vector<void*> FindInstancesOfVTable(void** vt, int maxHits);

// Every distinct plausible heap-object pointer stored in the main module's non-executable
// sections - i.e. the candidate set for "which static holds manager X". Deduplicated and
// filtered by alignment and user-address range; validity is the caller's problem.
std::vector<void*> ModuleStaticPointers();

// Demangled class name for a vtable, read back from the CompleteObjectLocator MSVC stores
// immediately before it. Answers "what did I actually find?" instead of guessing.
std::string RttiNameOfVTable(void** vt);
}
