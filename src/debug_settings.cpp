// SPDX-License-Identifier: GPL-3.0-or-later
#include "debug_settings.h"
#include "supported_builds.h"
#include "engine_dispatch.h"
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <utility>

namespace sm2debug {
namespace {
struct EnumChoice { std::int32_t value; std::string name, label; };
enum class AdjustmentPolicy { Default, PositiveBoolean, HalveDouble };
struct ParentFieldSpec {
    std::string field, descriptorName;
    std::uintptr_t live = 0, descriptor = 0, defaults = 0;
    std::uint32_t objectSize = 0, fieldCount = 0, index = 0, offset = 0, typeId = 0;
};
struct BindingSpec {
    std::string id, page, label, group, field, descriptorName;
    std::uintptr_t registration, live, descriptor, defaults;
    std::uint32_t objectSize, fieldCount, index, offset, code;
    DebugSettingType type;
    double step, minimum, maximum;
    std::string peer;
    int relation;
    std::uintptr_t enumDescriptor;
    std::uint32_t enumType;
    std::vector<EnumChoice> choices;
    ParentFieldSpec parent{};
    AdjustmentPolicy adjustment = AdjustmentPolicy::Default;
};
struct BindingAlias { std::string id, page, label; };
// Separate from reflected fields: exact original raw Boolean semantics,
// guarded by the entire matched retail consumer and its RIP-relative read.
struct RawBooleanSpec {
    std::string id, page, label;
    std::uintptr_t live = 0, consumer = 0;
    std::size_t readOffset = 0;
    std::vector<unsigned char> code;
};
#include "debug_settings_bindings.inc"
struct Image {
    std::uintptr_t base = 0;
    std::size_t size = 0;
    std::vector<std::pair<std::uintptr_t, std::size_t>> writable;
    std::vector<std::pair<std::uintptr_t, std::size_t>> executable;
    bool Contains(std::uintptr_t address, std::size_t count) const {
        return base && address >= base && count <= size && address - base <= size - count;
    }
    std::uintptr_t At(std::uintptr_t rva, std::size_t count = 1) const {
        return rva <= size && count <= size - rva && rva <= std::numeric_limits<std::uintptr_t>::max() - base ? base + rva : 0;
    }
};
struct Entry {
    BindingSpec spec;
    bool owned = false;
    std::uint32_t original = 0, applied = 0;
};
struct RawBooleanEntry {
    RawBooleanSpec spec;
    bool owned = false;
    unsigned char original = 0, applied = 0;
};
struct SettingsState {
    std::mutex mutex;
    bool initialized = false;
    Image image;
    std::vector<Entry> entries;
    std::vector<RawBooleanEntry> rawBooleans;
    std::vector<BindingAlias> aliases;
} settings;

bool ReadBytes(std::uintptr_t address, void* data, std::size_t size) {
    SIZE_T count = 0;
    return address >= 0x10000 && size && size <= std::numeric_limits<std::uintptr_t>::max() - address &&
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), data, size, &count) && count == size;
}
template<class T> bool Read(std::uintptr_t address, T& value) { return ReadBytes(address, &value, sizeof(value)); }
template<class T> bool ReadImage(const Image& image, std::uintptr_t address, T& value) {
    return image.Contains(address, sizeof(value)) && Read(address, value);
}
bool SameString(const Image& image, std::uintptr_t address, const std::string& expected) {
    if (expected.empty() || expected.size() > 180 || !image.Contains(address, expected.size() + 1)) return false;
    std::array<char, 181> buffer{};
    return ReadBytes(address, buffer.data(), expected.size() + 1) &&
        std::memcmp(buffer.data(), expected.c_str(), expected.size() + 1) == 0;
}
bool WritableScalar(const Image& image, std::uintptr_t address, std::size_t size) {
    if (!image.Contains(address, size) || (size > 1 && address % size)) return false;
    const bool inSection = std::any_of(image.writable.begin(), image.writable.end(), [&](const auto& section) {
        return address >= section.first && size <= section.second && address - section.first <= section.second - size;
    });
    MEMORY_BASIC_INFORMATION memory{};
    if (!inSection || VirtualQuery(reinterpret_cast<void*>(address), &memory, sizeof(memory)) != sizeof(memory) ||
        memory.State != MEM_COMMIT || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
        ((memory.Protect & 0xff) != PAGE_READWRITE && (memory.Protect & 0xff) != PAGE_WRITECOPY)) return false;
    const auto region = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
    return address >= region && size <= memory.RegionSize && address - region <= memory.RegionSize - size;
}
bool CurrentRawBoolean(const Image& image, const RawBooleanSpec& spec, std::uintptr_t& address, unsigned char& value) {
    if (spec.code.size() < 7 || spec.code.size() > 1024 || spec.readOffset > spec.code.size()-7 ||
        spec.code[spec.readOffset] != 0x80 || spec.code[spec.readOffset+1] != 0x3d || spec.code[spec.readOffset+6] != 0) return false;
    const auto codeAddress=image.At(spec.consumer,spec.code.size());
    address=image.At(spec.live);
    std::int32_t displacement=0;std::memcpy(&displacement,spec.code.data()+spec.readOffset+2,4);
    if (!codeAddress || !address || spec.consumer > static_cast<std::uintptr_t>(std::numeric_limits<std::int64_t>::max())-spec.code.size() ||
        static_cast<std::int64_t>(spec.consumer+spec.readOffset+7)+displacement != static_cast<std::int64_t>(spec.live) ||
        !WritableScalar(image,address,1)) return false;
    const bool inCode=std::any_of(image.executable.begin(),image.executable.end(),[&](const auto& section) {
        return codeAddress>=section.first&&spec.code.size()<=section.second&&codeAddress-section.first<=section.second-spec.code.size();
    });
    MEMORY_BASIC_INFORMATION memory{};
    if (!inCode || VirtualQuery(reinterpret_cast<void*>(codeAddress),&memory,sizeof(memory))!=sizeof(memory) ||
        memory.State!=MEM_COMMIT || (memory.Protect&(PAGE_GUARD|PAGE_NOACCESS)) ||
        (memory.Protect&0xff)!=PAGE_EXECUTE_READ) return false;
    const auto region=reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
    std::array<unsigned char,1024> actual{};
    return codeAddress>=region&&spec.code.size()<=memory.RegionSize&&codeAddress-region<=memory.RegionSize-spec.code.size()&&
        ReadBytes(codeAddress,actual.data(),spec.code.size())&&std::equal(spec.code.begin(),spec.code.end(),actual.begin())&&
        Read(address,value)&&value<=1;
}
bool ExchangeRawBoolean(const Image& image, const RawBooleanSpec& spec, unsigned char expected, unsigned char desired) {
    if (expected>1||desired>1||!IsWithinVerifiedEngineCallback()) return false;
    std::uintptr_t address=0;unsigned char current=0;
    if (!CurrentRawBoolean(image,spec,address,current)||current!=expected) return false;
    return __atomic_compare_exchange_n(reinterpret_cast<volatile unsigned char*>(address),&expected,desired,
        false,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);
}
bool ArrayAddress(const Image& image, std::uintptr_t descriptor, std::size_t member,
    std::size_t index, std::size_t stride, std::size_t width, std::uintptr_t& result) {
    std::uintptr_t pointer = 0;
    if (!ReadImage(image, descriptor + member, pointer) || index > std::numeric_limits<std::size_t>::max() / stride) return false;
    const std::size_t offset = index * stride;
    if (offset > std::numeric_limits<std::uintptr_t>::max() - pointer) return false;
    result = pointer + offset;
    return image.Contains(result, width);
}
std::size_t Width(const BindingSpec& spec) {
    return spec.type == DebugSettingType::Boolean ? 1 : spec.type == DebugSettingType::Int16 ? 2 : 4;
}
bool ValidateParent(const Image& image, const BindingSpec& spec) {
    const auto& parent = spec.parent;
    if (!parent.descriptor) return true;
    if (!parent.objectSize || !parent.fieldCount || parent.fieldCount > 4096 || parent.index >= parent.fieldCount ||
        parent.offset >= parent.objectSize || spec.objectSize > parent.objectSize - parent.offset ||
        parent.live == parent.defaults || !parent.typeId ||
        parent.offset > std::numeric_limits<std::uintptr_t>::max() - parent.live ||
        spec.live != parent.live + parent.offset) return false;
    const auto descriptor = image.At(parent.descriptor, 0xb0);
    const auto live = image.At(parent.live, parent.objectSize);
    const auto child = image.At(spec.descriptor, 0xb0);
    std::uintptr_t name = 0, defaults = 0, item = 0;
    std::uint32_t size = 0, count = 0, offset = 0, typeId = 0;
    std::uint16_t fieldCount = 0;
    std::uint8_t code = 0;
    return descriptor && live && child &&
        ReadImage(image, descriptor, name) && SameString(image, name, parent.descriptorName) &&
        ReadImage(image, descriptor + 0x14, size) && size == parent.objectSize &&
        ReadImage(image, descriptor + 0x18, fieldCount) && fieldCount == parent.fieldCount &&
        ReadImage(image, descriptor + 0x50, defaults) && defaults == image.At(parent.defaults, parent.objectSize) && defaults != live &&
        ArrayAddress(image, descriptor, 0x60, parent.index, 8, 8, item) && ReadImage(image, item, name) && SameString(image, name, parent.field) &&
        ArrayAddress(image, descriptor, 0x68, parent.index, 1, 1, item) && ReadImage(image, item, code) && code == 13 &&
        ArrayAddress(image, descriptor, 0x80, parent.index, 4, 4, item) && ReadImage(image, item, count) && count == 1 &&
        ArrayAddress(image, descriptor, 0x88, parent.index, 4, 4, item) && ReadImage(image, item, offset) && offset == parent.offset &&
        ArrayAddress(image, descriptor, 0x90, parent.index, 4, 4, item) && ReadImage(image, item, typeId) && typeId == parent.typeId &&
        ReadImage(image, child + 8, typeId) && typeId == parent.typeId;
}
bool Validate(const Image& image, const BindingSpec& spec, std::uintptr_t& address) {
    const unsigned expectedCode = spec.type == DebugSettingType::Boolean ? 15 : spec.type == DebugSettingType::Int32 ? 6 :
        spec.type == DebugSettingType::UInt32 ? 2 : spec.type == DebugSettingType::Float32 ? 8 :
        spec.type == DebugSettingType::Int16 ? 5 : 11;
    if (spec.index >= spec.fieldCount || !spec.fieldCount || spec.fieldCount > 4096 ||
        spec.offset >= spec.objectSize || Width(spec) > spec.objectSize - spec.offset ||
        spec.code != expectedCode || spec.live == spec.defaults || spec.offset > std::numeric_limits<std::uintptr_t>::max() - spec.live) return false;
    if (!ValidateParent(image, spec)) return false;
    const auto registration = image.At(spec.registration, 24), descriptor = image.At(spec.descriptor, 0xb0);
    const auto live = image.At(spec.live, spec.objectSize);
    const auto registeredLive = spec.parent.descriptor ? image.At(spec.parent.live, spec.parent.objectSize) : live;
    const auto registeredDescriptor = spec.parent.descriptor ? image.At(spec.parent.descriptor, 0xb0) : descriptor;
    std::uintptr_t name = 0, recordLive = 0, recordDescriptor = 0, defaults = 0;
    std::uint32_t objectSize = 0;
    std::uint16_t fieldCount = 0;
    if (!registration || !descriptor || !live || !ReadImage(image, registration, name) || !SameString(image, name, spec.group) ||
        !ReadImage(image, registration + 8, recordLive) || recordLive != registeredLive ||
        !ReadImage(image, registration + 16, recordDescriptor) || recordDescriptor != registeredDescriptor ||
        !ReadImage(image, descriptor, name) || !SameString(image, name, spec.descriptorName) ||
        !ReadImage(image, descriptor + 0x14, objectSize) || objectSize != spec.objectSize ||
        !ReadImage(image, descriptor + 0x18, fieldCount) || fieldCount != spec.fieldCount ||
        !ReadImage(image, descriptor + 0x50, defaults) || defaults != image.At(spec.defaults, spec.objectSize) || defaults == live) return false;
    std::uintptr_t item = 0;
    std::uint8_t code = 0;
    std::uint32_t count = 0, offset = 0;
    if (!ArrayAddress(image, descriptor, 0x60, spec.index, 8, 8, item) || !ReadImage(image, item, name) || !SameString(image, name, spec.field) ||
        !ArrayAddress(image, descriptor, 0x68, spec.index, 1, 1, item) || !ReadImage(image, item, code) || code != spec.code ||
        !ArrayAddress(image, descriptor, 0x80, spec.index, 4, 4, item) || !ReadImage(image, item, count) || count != 1 ||
        !ArrayAddress(image, descriptor, 0x88, spec.index, 4, 4, item) || !ReadImage(image, item, offset) || offset != spec.offset) return false;
    if (spec.type == DebugSettingType::Enum32) {
        std::uint32_t type = 0, actualCount = 0;
        const auto enumDescriptor = image.At(spec.enumDescriptor, 0x48);
        if (spec.choices.empty() || spec.choices.size() > 64 || !enumDescriptor ||
            !ArrayAddress(image, descriptor, 0x90, spec.index, 4, 4, item) || !ReadImage(image, item, type) || type != spec.enumType ||
            !ReadImage(image, enumDescriptor + 8, type) || type != spec.enumType ||
            !ReadImage(image, enumDescriptor + 12, actualCount) || actualCount != spec.choices.size()) return false;
        for (std::size_t i = 0; i < spec.choices.size(); ++i) {
            if (spec.choices[i].value != static_cast<std::int32_t>(i) ||
                !ArrayAddress(image, enumDescriptor, 0x28, i, 8, 8, item) || !ReadImage(image, item, name) || !SameString(image, name, spec.choices[i].name) ||
                !ArrayAddress(image, enumDescriptor, 0x38, i, 8, 8, item) || !ReadImage(image, item, name) || !SameString(image, name, spec.choices[i].label)) return false;
        }
    } else if (spec.enumDescriptor || spec.enumType || !spec.choices.empty()) return false;
    address = image.At(spec.live + spec.offset, Width(spec));
    return address && WritableScalar(image, address, Width(spec));
}
bool Decode(const BindingSpec& spec, std::uint32_t bits, double& value) {
    switch (spec.type) {
    case DebugSettingType::Boolean: if (bits > 1) return false; value = bits; break;
    case DebugSettingType::Int16: { std::int16_t number = 0; std::memcpy(&number, &bits, 2); value = number; break; }
    case DebugSettingType::Int32: { std::int32_t number = 0; std::memcpy(&number, &bits, 4); value = number; break; }
    case DebugSettingType::UInt32: value = bits; break;
    case DebugSettingType::Float32: { float number = 0; std::memcpy(&number, &bits, 4); if (!std::isfinite(number)) return false; value = number; break; }
    case DebugSettingType::Enum32:
        if (bits >= spec.choices.size()) return false;
        value = spec.choices[bits].value; break;
    }
    return true;
}
bool Current(const Image& image, const BindingSpec& spec, std::uintptr_t& address, std::uint32_t& bits, double& value) {
    bits = 0;
    return Validate(image, spec, address) && ReadBytes(address, &bits, Width(spec)) && Decode(spec, bits, value);
}
bool CompareExchange(const Image& image, const BindingSpec& spec, std::uintptr_t address, std::uint32_t expected, std::uint32_t desired) {
    if (!IsWithinVerifiedEngineCallback() || !WritableScalar(image, address, Width(spec))) return false;
    if (Width(spec) == 1) {
        auto oldByte = static_cast<unsigned char>(expected);
        return __atomic_compare_exchange_n(reinterpret_cast<volatile unsigned char*>(address), &oldByte,
            static_cast<unsigned char>(desired), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
    if (Width(spec) == 2) {
        auto oldWord = static_cast<std::uint16_t>(expected);
        return __atomic_compare_exchange_n(reinterpret_cast<volatile std::uint16_t*>(address), &oldWord,
            static_cast<std::uint16_t>(desired), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
    LONG oldBits = 0, newBits = 0;
    std::memcpy(&oldBits, &expected, 4); std::memcpy(&newBits, &desired, 4);
    return InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(address), newBits, oldBits) == oldBits;
}
std::string HashFile(const std::filesystem::path& path) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    DWORD bytes = 0, returned = 0;
    bool ok = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&bytes), sizeof(bytes), &returned, 0) >= 0;
    std::vector<unsigned char> object(bytes), buffer(1024 * 1024), digest(32);
    ok = ok && BCryptCreateHash(algorithm, &hash, object.data(), bytes, nullptr, 0, 0) >= 0;
    std::ifstream file(path, std::ios::binary); ok = ok && file.is_open();
    while (ok && file) {
        file.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
        if (file.gcount() && BCryptHashData(hash, buffer.data(), static_cast<ULONG>(file.gcount()), 0) < 0) ok = false;
    }
    if (file.bad()) ok = false;
    ok = ok && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) return {};
    std::ostringstream result;
    for (unsigned char byte : digest) result << std::hex << std::setw(2) << std::setfill('0') << unsigned(byte);
    return result.str();
}
bool GameImage(Image& image) {
    const auto module = GetModuleHandleW(L"Spider-Man2.exe");
    if (!module || module != GetModuleHandleW(nullptr)) return false;
    wchar_t path[32768]{};
    if (!GetModuleFileNameW(module, path, 32768) || !SupportedExecutable::IsSupported(HashFile(path))) return false;
    image.base = reinterpret_cast<std::uintptr_t>(module);
    IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS64 nt{};
    if (!Read(image.base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || dos.e_lfanew > 0x100000 ||
        !Read(image.base + dos.e_lfanew, nt) || nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || nt.OptionalHeader.SizeOfImage != 0xce54000 ||
        !nt.FileHeader.NumberOfSections || nt.FileHeader.NumberOfSections > 96) return false;
    image.size = nt.OptionalHeader.SizeOfImage;
    const auto table = image.base + dos.e_lfanew + 24 + nt.FileHeader.SizeOfOptionalHeader;
    for (unsigned i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
        IMAGE_SECTION_HEADER section{};
        if (!ReadImage(image, table + i * sizeof(section), section)) return false;
        if ((section.Characteristics & IMAGE_SCN_MEM_WRITE) && !(section.Characteristics & IMAGE_SCN_MEM_EXECUTE)) {
            const auto start = image.At(section.VirtualAddress, section.Misc.VirtualSize);
            if (!start) return false;
            image.writable.push_back({start, section.Misc.VirtualSize});
        }
        if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) && !(section.Characteristics & IMAGE_SCN_MEM_WRITE)) {
            const auto start=image.At(section.VirtualAddress,section.Misc.VirtualSize);
            if (!start) return false;
            image.executable.push_back({start,section.Misc.VirtualSize});
        }
    }
    return !image.writable.empty();
}
void InitializeEntries(const Image& image, const std::vector<BindingSpec>& specs,
    const std::vector<BindingAlias>& aliases = {}, const std::vector<RawBooleanSpec>& rawBooleans = {}) {
    settings.image = image; settings.entries.clear();settings.rawBooleans.clear();
    settings.aliases = aliases;
    for (const auto& spec : specs) settings.entries.push_back({spec});
    for (const auto& spec : rawBooleans) settings.rawBooleans.push_back({spec});
    settings.initialized = true;
}
DebugSettingValue ReadEntry(const Entry& entry) {
    DebugSettingValue result;
    result.type = entry.spec.type; result.id = entry.spec.id;
    result.pageId = entry.spec.page; result.label = entry.spec.label;
    std::uintptr_t address = 0; std::uint32_t bits = 0;
    result.available = Current(settings.image, entry.spec, address, bits, result.number);
    if (result.available) for (const auto& choice : entry.spec.choices) {
        const auto duplicate = std::count_if(entry.spec.choices.begin(), entry.spec.choices.end(),
            [&](const auto& other) { return other.label == choice.label; }) > 1;
        // Some DDL enums omit friendly labels and repeat the NONE sentinel.
        // Keep validating those exact metadata labels; present the distinct
        // retained member names instead of inventing friendly descriptions.
        result.choices.push_back(choice.label == "NONE" || duplicate ? choice.name : choice.label);
    }
    return result;
}
DebugSettingValue ReadRawBoolean(const RawBooleanEntry& entry) {
    DebugSettingValue result;result.type=DebugSettingType::Boolean;result.id=entry.spec.id;
    result.pageId=entry.spec.page;result.label=entry.spec.label;
    std::uintptr_t address=0;unsigned char value=0;
    result.available=CurrentRawBoolean(settings.image,entry.spec,address,value);result.number=value;
    return result;
}
} // namespace

bool InitializeDebugSettings(HMODULE module) {
    (void)module;
    try {
        std::lock_guard lock(settings.mutex);
        if (settings.initialized) return true;
        Image image;
        if (!GameImage(image)) return false;
        InitializeEntries(image, kBindings, kBindingAliases, kRawBooleans);
        return true;
    } catch (...) { return false; }
}
DebugSettingValue ReadDebugSetting(const std::string& pageId, const std::string& label) {
    DebugSettingValue result;
    try {
        std::lock_guard lock(settings.mutex);
        if (!settings.initialized) return result;
        for (const auto& entry : settings.entries) if (entry.spec.page == pageId && entry.spec.label == label) {
            return ReadEntry(entry);
        }
        for (const auto& alias : settings.aliases) if (alias.page == pageId && alias.label == label) {
            for (const auto& entry : settings.entries) if (entry.spec.id == alias.id) {
                result = ReadEntry(entry); result.pageId = pageId; result.label = label; return result;
            }
        }
        for (const auto& entry : settings.rawBooleans) if (entry.spec.page==pageId&&entry.spec.label==label) return ReadRawBoolean(entry);
    } catch (...) {}
    return result;
}
std::vector<DebugSettingValue> SnapshotDebugSettings() {
    std::vector<DebugSettingValue> result;
    try {
        std::lock_guard lock(settings.mutex);
        if (!settings.initialized) return result;
        result.reserve(settings.entries.size() + settings.aliases.size() + settings.rawBooleans.size());
        for (const auto& entry : settings.entries) {
            auto value = ReadEntry(entry); result.push_back(value);
            for (const auto& alias : settings.aliases) if (alias.id == entry.spec.id) {
                auto copy = value; copy.pageId = alias.page; copy.label = alias.label; result.push_back(std::move(copy));
            }
        }
        for (const auto& entry : settings.rawBooleans) result.push_back(ReadRawBoolean(entry));
    } catch (...) { result.clear(); }
    return result;
}
bool ApplyDebugSetting(const std::string& id, int direction) {
    try {
        if (direction < -1 || direction > 1 || !IsWithinVerifiedEngineCallback()) return false;
        std::unique_lock lock(settings.mutex, std::try_to_lock);
        if (!lock || !settings.initialized) return false;
        for (auto& entry : settings.rawBooleans) if (entry.spec.id==id) {
            std::uintptr_t address=0;unsigned char value=0;
            if (!CurrentRawBoolean(settings.image,entry.spec,address,value)) return false;
            const auto next=static_cast<unsigned char>(direction ? direction>0 : !value);
            if (value==next) return true;
            if (!ExchangeRawBoolean(settings.image,entry.spec,value,next)) return false;
            if (!entry.owned||value!=entry.applied) entry.original=value;
            entry.applied=next;entry.owned=true;return true;
        }
        for (auto& entry : settings.entries) if (entry.spec.id == id) {
            auto& spec = entry.spec;
            if ((spec.adjustment == AdjustmentPolicy::PositiveBoolean && spec.type != DebugSettingType::Boolean) ||
                (spec.adjustment == AdjustmentPolicy::HalveDouble && spec.type != DebugSettingType::UInt32) ||
                (spec.adjustment != AdjustmentPolicy::Default && spec.adjustment != AdjustmentPolicy::PositiveBoolean &&
                 spec.adjustment != AdjustmentPolicy::HalveDouble)) return false;
            std::uintptr_t address = 0; std::uint32_t bits = 0, desired = 0; double value = 0;
            if (!Current(settings.image, spec, address, bits, value)) return false;
            double next = value;
            if (spec.type == DebugSettingType::Boolean) next = spec.adjustment == AdjustmentPolicy::PositiveBoolean ?
                (direction > 0 ? 1 : 0) : direction ? (direction > 0 ? 1 : 0) : (value == 0 ? 1 : 0);
            else if (spec.type == DebugSettingType::Enum32) {
                // Validate the full native enum for reads/restores, but cycle
                // only the reviewed write range. Some retained enum members
                // have no established runtime implementation in this build.
                if (!std::isfinite(spec.minimum) || !std::isfinite(spec.maximum) ||
                    spec.minimum < 0 || spec.minimum >= spec.choices.size() || spec.maximum >= spec.choices.size() || spec.maximum < spec.minimum ||
                    std::floor(spec.minimum) != spec.minimum || std::floor(spec.maximum) != spec.maximum) return false;
                const int first = static_cast<int>(spec.minimum);
                const int last = static_cast<int>(spec.maximum);
                const int count = last - first + 1;
                if (value < first || value > last) next = direction < 0 ? last : first;
                else next = first + (static_cast<int>(value) - first + (direction < 0 ? -1 : 1) + count) % count;
            } else {
                if (!direction) return false;
                const double primitiveMin = spec.type == DebugSettingType::Float32 ? -std::numeric_limits<float>::max() :
                    spec.type == DebugSettingType::UInt32 ? 0 : spec.type == DebugSettingType::Int16 ? -32768.0 : -2147483648.0;
                const double primitiveMax = spec.type == DebugSettingType::Float32 ? std::numeric_limits<float>::max() :
                    spec.type == DebugSettingType::UInt32 ? 4294967295.0 : spec.type == DebugSettingType::Int16 ? 32767.0 : 2147483647.0;
                if (!std::isfinite(spec.step) || spec.step <= 0 || !std::isfinite(spec.minimum) ||
                    !std::isfinite(spec.maximum) || spec.minimum > spec.maximum || spec.minimum < primitiveMin ||
                    spec.maximum > primitiveMax || (spec.type != DebugSettingType::Float32 &&
                    (std::floor(spec.step) != spec.step || std::floor(spec.minimum) != spec.minimum || std::floor(spec.maximum) != spec.maximum))) return false;
                if (spec.adjustment == AdjustmentPolicy::HalveDouble) {
                    // Native custom terrain control halves/doubles, clamps to
                    // its reviewed endpoints, and ignores activation without a direction.
                    if (value < spec.minimum || value > spec.maximum) return false;
                    next = direction < 0 ? std::floor(value / 2) : value * 2;
                    next = std::max(spec.minimum, std::min(spec.maximum, next));
                } else next += direction * spec.step;
                if (!std::isfinite(next) || next < spec.minimum || next > spec.maximum) return false;
                if (!spec.peer.empty()) {
                    auto peer = std::find_if(settings.entries.begin(), settings.entries.end(), [&](const auto& e) { return e.spec.id == spec.peer; });
                    std::uintptr_t peerAddress = 0; std::uint32_t peerBits = 0; double peerValue = 0;
                    if (peer == settings.entries.end() || !Current(settings.image, peer->spec, peerAddress, peerBits, peerValue) ||
                        (spec.relation < 0 && next > peerValue) || (spec.relation > 0 && next < peerValue)) return false;
                }
            }
            if (spec.type == DebugSettingType::Float32) { float scalar = static_cast<float>(next); if (!std::isfinite(scalar)) return false; std::memcpy(&desired, &scalar, 4); }
            else if (spec.type == DebugSettingType::Int16) { auto scalar = static_cast<std::int16_t>(next); std::memcpy(&desired, &scalar, 2); }
            else if (spec.type == DebugSettingType::Int32) { auto scalar = static_cast<std::int32_t>(next); std::memcpy(&desired, &scalar, 4); }
            else desired = static_cast<std::uint32_t>(next);
            if (desired == bits) return true;
            if (!CompareExchange(settings.image, spec, address, bits, desired)) return false;
            // A later explicit edit starts from any external value observed
            // since our previous edit. Restore never overwrites that baseline.
            if (!entry.owned || bits != entry.applied) entry.original = bits;
            entry.applied = desired; entry.owned = true;
            return true;
        }
    } catch (...) {}
    return false;
}
bool RestoreDebugSettings() {
    try {
        if (!IsWithinVerifiedEngineCallback()) return false;
        std::unique_lock lock(settings.mutex, std::try_to_lock);
        if (!lock) return false; // Ownership remains pending for a later safe-point retry.
        for (auto& entry : settings.rawBooleans) if (entry.owned) {
            std::uintptr_t address=0;unsigned char value=0;
            if (!CurrentRawBoolean(settings.image,entry.spec,address,value)) continue;
            if (value!=entry.applied) {entry.owned=false;continue;}
            if (ExchangeRawBoolean(settings.image,entry.spec,value,entry.original)) entry.owned=false;
        }
        for (auto& entry : settings.entries) if (entry.owned) {
            std::uintptr_t address = 0; std::uint32_t bits = 0; double value = 0;
            if (!Current(settings.image, entry.spec, address, bits, value)) continue;
            if (bits != entry.applied) { entry.owned = false; continue; }
            if (!entry.spec.peer.empty()) {
                auto peer = std::find_if(settings.entries.begin(), settings.entries.end(), [&](const auto& e) { return e.spec.id == entry.spec.peer; });
                std::uintptr_t peerAddress = 0; std::uint32_t peerBits = 0; double peerValue = 0, originalValue = 0;
                if (peer == settings.entries.end() || !Decode(entry.spec, entry.original, originalValue) ||
                    !Current(settings.image, peer->spec, peerAddress, peerBits, peerValue) ||
                    (entry.spec.relation < 0 && originalValue > peerValue) ||
                    (entry.spec.relation > 0 && originalValue < peerValue)) continue;
            }
            if (CompareExchange(settings.image, entry.spec, address, bits, entry.original)) entry.owned = false;
        }
        return std::none_of(settings.entries.begin(), settings.entries.end(), [](const auto& entry) { return entry.owned; })&&
            std::none_of(settings.rawBooleans.begin(),settings.rawBooleans.end(),[](const auto& entry){return entry.owned;});
    } catch (...) {}
    return false;
}
} // namespace sm2debug
