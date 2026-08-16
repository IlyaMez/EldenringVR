#pragma once
#include <cstdint>

namespace ervr {
// Finds the primary virtual function table of an MSVC RTTI-enabled class in the
// main module by its demangled type-descriptor name, e.g. "CS::NoUpdateInterface"
// (matched against ".?AVNoUpdateInterface@CS@@"). Returns nullptr if not found.
void** FindVTable(const char* className);
}
