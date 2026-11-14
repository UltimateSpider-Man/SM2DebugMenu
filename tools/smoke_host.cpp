// A plain native host for the installed DLL's real C loader boundary.
// It is deliberately not named Spider-Man2.exe: gameplay must stay disabled.
#include <windows.h>
#include <filesystem>

LRESULT CALLBACK HostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    WNDCLASSW cls{};
    cls.lpfnWndProc = HostProc;
    cls.hInstance = instance;
    cls.lpszClassName = L"SM2DebugMenuSmokeHost";
    if (!RegisterClassW(&cls)) return 1;
    HWND host = CreateWindowExW(0, cls.lpszClassName, L"SM2 Debug Menu smoke host",
        WS_OVERLAPPEDWINDOW, 100, 100, 1050, 760, nullptr, nullptr, instance, nullptr);
    if (!host) return 2;
    // The controller displays the interactive test host after STARTUPINFO's
    // initial hidden state. The native menu must discover only this host HWND.
    ShowWindow(host, SW_SHOW);
    wchar_t executable[32768]{};
    if (!GetModuleFileNameW(nullptr, executable, 32768)) return 3;
    auto path = std::filesystem::path(executable).parent_path() / L"sm2debugmenu.dll";
    HMODULE dll = LoadLibraryW(path.c_str());
    if (!dll) return 4;
    auto enable = reinterpret_cast<void (*)()>(GetProcAddress(dll, "script_enable"));
    if (!enable) return 5;
    enable(); enable(); // A second enable call must not create a duplicate menu.
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    // The DLL has a persistent UI worker, like the game's loader. Let process
    // teardown own unloading rather than unloading code under that worker.
    return 0;
}
