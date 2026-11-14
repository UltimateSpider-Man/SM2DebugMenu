// Uses the production scalar file hasher and compiled exact-admission policy.
// Inputs are only read as data; no executable is loaded or started.
#include "../src/debug_settings.cpp"
#include <iostream>
namespace sm2debug { bool IsWithinVerifiedEngineCallback() noexcept { return false; } }
int wmain(int argc,wchar_t** argv) {
    if(argc!=2)return 2;
    const auto hash=sm2debug::HashFile(std::filesystem::path(argv[1]));
    std::cout<<"{\"sha256\":\""<<hash<<"\",\"supported\":"
        <<(SupportedExecutable::IsSupported(hash)?"true":"false")<<"}\n";
    return hash.empty()?3:0;
}
