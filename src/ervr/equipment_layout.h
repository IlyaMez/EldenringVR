#pragma once
// Where the player's equipment lives, shared by combat and the weapon wheel. Offsets follow the field
// order of fromsoftware-rs PlayerIns / ChrAsm / EQUIP_PARAM_WEAPON_ST (read-only reference); in use
// since the 2.6.2.0 combat work and unchanged on 2.7.2.0.
#include <cstddef>

namespace ervr {

constexpr size_t Player_ChrAsm     = 0x638;   // PlayerIns -> ChrAsm*
constexpr size_t Asm_ArmStyle      = 0x08;    // 0 empty, 1 one-handed, 2 left both hands, 3 right both hands
constexpr size_t Asm_LeftSlot      = 0x0C;    // active weapon slot of each hand, 0..2
constexpr size_t Asm_RightSlot     = 0x10;
constexpr size_t Asm_GaitemHandles = 0x24;    // GaitemHandle[22], ChrAsmEquipEntries order (bit 23 = indexed, bits 0-15 = CSGaitemImp table index)
// equipment_param_ids[22] in ChrAsmEquipEntries order: 0 primary-left, 1 primary-right,
// 2 secondary-left, 3 secondary-right, 4/5 tertiary - so slot k of hand h is entry k * 2 + h.
constexpr size_t Asm_ParamIds      = 0x7C;

// EQUIP_PARAM_WEAPON_ST
constexpr size_t Wep_IconId        = 0xBE;    // u16 iconId: the menu icon texture MENU_Knowledge_<iconId>
constexpr size_t Wep_Type          = 0x1A6;   // u16 wepType (65 small / 67 medium / 69 great shield)

} // namespace ervr
