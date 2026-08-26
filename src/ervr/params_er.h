#pragma once
#include <cstdint>

namespace ervr {

// Runtime access to the game's loaded regulation params (CS::SoloParamRepository). Rows are
// the raw in-memory PARAM file data, so writing a field changes what the game reads from
// then on - the same live-edit the param patchers in the CE community rely on. Nothing on
// disk is touched and every edit ERVR makes is restored by its owner.
//
// Holder indices are the game's own SoloParamRepository order (fromsoftware-rs, ER 1.16).
enum ParamIndex {
    PI_EquipParamWeapon = 0,
    PI_AtkParam_Pc      = 8,
    PI_BehaviorParam_PC = 13,
    PI_SpEffectParam    = 15,
    PI_SwordArtsParam   = 82,
    PI_EquipParamGem    = 154,
};

void ParamsInit();          // resolve the repository static (AOB); safe to call before params load
bool ParamsReady();         // repository located AND content-validated (lazy; cheap once true)
// Row data for `rowId` in holder `index`, or null. `rowSize` (optional) receives the row stride.
uint8_t* ParamsRow(int index, uint32_t rowId, uint32_t* rowSize = nullptr);
// Row data by descriptor position (0..ParamsRowCount-1), for scanning a param by field content.
uint8_t* ParamsRowAt(int index, uint32_t position, uint32_t* rowId);
const char* ParamsStructName(int index);   // e.g. "ATK_PARAM_ST"; "" when unavailable
uint32_t ParamsRowCount(int index);
}
