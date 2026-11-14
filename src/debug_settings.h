// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace sm2debug {
enum class DebugSettingType { Boolean, Int32, UInt32, Float32, Enum32, Int16 };
struct DebugSettingValue {
    bool available = false;
    DebugSettingType type = DebugSettingType::Boolean;
    double number = 0;
    std::string id, pageId, label;
    std::vector<std::string> choices;
};
bool InitializeDebugSettings(HMODULE module);
DebugSettingValue ReadDebugSetting(const std::string& pageId, const std::string& label);
std::vector<DebugSettingValue> SnapshotDebugSettings();
bool ApplyDebugSetting(const std::string& id, int direction);
// False retains ownership for a later verified-frame retry.
bool RestoreDebugSettings();
} // namespace sm2debug
