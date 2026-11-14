// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "reference_catalog.h"
#include <cstdint>
#include <string>
#include <vector>

namespace sm2debug {
// These are typed serialized values, never allocator-owned native option objects.
// An omitted parameter is absent from this vector and retains its engine default.
enum class NativeDevValueKind { String, Boolean, Object, Array };
struct NativeDevParameter {
    std::string name;
    NativeDevValueKind kind = NativeDevValueKind::String;
    std::string text;
    bool boolean = false;
    // Object children retain their original names. Array children have no name.
    std::vector<NativeDevParameter> children;
};
struct NativeDevAssetDependency {
    std::string parameter, path;
    std::uint64_t assetId = 0;
    bool tocPresent = false;
};
struct NativeDevOption {
    std::string id, pageId, label, nativeType;
    std::uint32_t nativeTypeId = 0;
    std::string targetPageId;
    std::vector<NativeDevParameter> parameters;
    std::vector<NativeDevAssetDependency> assets;
    bool missingAssets = false;
    bool missingMenu = false;
    // Metadata and asset membership do not establish a callable native binding.
    bool runtimeCallableVerified = false;
};

inline constexpr char kNativeDevRoot[] = "dev.retail";
inline constexpr char kNativeDevExeSha256[] =
    "54215324e8c1e07c95c832a2d2d7b340d9028ca107ca4e5ad1072cd2809b7975";
inline constexpr char kNativeDevConfigAsset[] =
    "configs/system/system_TitleScreenOptions.config";
inline constexpr char kNativeDevConfigSha256[] =
    "a906d9345fdc2f83a1024e1ad457c6c85e074b8a5bf4292697ea4e2830b184b0";

// Exactly the 25 retained config menus and their 183 ordered options. Navigation
// is local; every native gameplay leaf has an empty ReferenceRow target.
const std::vector<ReferencePage>& NativeDevPages();
const std::vector<NativeDevOption>& NativeDevOptions();
const NativeDevOption* FindNativeDevOption(const std::string& pageId,
                                         const std::string& label);
} // namespace sm2debug
