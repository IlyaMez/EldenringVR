#pragma once
#include <memory>
#include <string>
#include "imgui.h"
struct ID3D11Device;
namespace launcher {
class WeaponWorkshop {
  public:
    explicit WeaponWorkshop(ID3D11Device* device);
    ~WeaponWorkshop();
    void draw(const std::wstring& gameDirectory, const std::wstring& ini, ImVec2 origin, ImVec2 size, float scale);

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace launcher
