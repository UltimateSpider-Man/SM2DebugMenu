// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <vector>

namespace sm2debug {
struct ReferenceRow {
    std::string label;
    std::string target;
    bool heading = false;
    std::string help{};
};
struct ReferencePage {
    std::string id, title;
    bool developer = false;
    std::vector<ReferenceRow> rows;
};
const std::vector<ReferencePage>& ReferencePages();
const ReferencePage* FindReferencePage(const std::string& id);
} // namespace sm2debug
