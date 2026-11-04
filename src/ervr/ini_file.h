#pragma once
// ERVR.ini as text, for the parts of ERVR.dll that write to it while the game runs (the ERVR tab, the body
// calibration): read whole, edited with launcher::text::iniSet, written whole and renamed into place, so the
// hot reload (main.cpp) never reads half a file.
#include <windows.h>
#include <string>

namespace ervr {

inline std::string IniReadAll(const std::wstring& path) {
    std::string out;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return out;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < (16 << 20)) {
        out.resize((size_t)size.QuadPart);
        DWORD got = 0;
        if (!ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr))
            got = 0;
        out.resize(got);
    }
    CloseHandle(h);
    return out;
}

// `temp` names the file written first, beside `path` (each writer its own, so two never share one).
inline bool IniWriteAll(const std::wstring& path, const std::string& text, const wchar_t* temp) {
    const std::wstring scratch = path + temp;
    HANDLE h = CreateFileW(scratch.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    DWORD put = 0;
    const bool ok = WriteFile(h, text.data(), (DWORD)text.size(), &put, nullptr) && put == text.size();
    CloseHandle(h);
    if (ok && MoveFileExW(scratch.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return true;
    DeleteFileW(scratch.c_str());
    return false;
}

} // namespace ervr
