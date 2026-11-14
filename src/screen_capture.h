// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>
struct ID3D12Device;
struct ID3D12CommandQueue;
struct IDXGISwapChain3;
namespace sm2debug {
enum class CaptureImageFormat : unsigned { Bmp, Dds, Jpg, Png };
struct ScreenCaptureState {
    bool ready = false, busy = false;
    CaptureImageFormat format = CaptureImageFormat::Bmp;
    std::uint64_t completed = 0;
    std::wstring status = L"Waiting for the game display";
    std::wstring lastPath;
};
ScreenCaptureState ReadScreenCaptureState();
void SetScreenCaptureAvailable(bool available, const std::wstring& reason);
bool CycleCaptureImageFormat(int direction);
bool RequestDisplayCapture();
// Called only by the owner of the actual presentation queue, before drawing
// this overlay. A request waits until the menu has closed; the game HUD stays.
void ProcessScreenCapture(ID3D12Device*, ID3D12CommandQueue*, IDXGISwapChain3*,
                          const std::wstring& directory, bool menuVisible);
// Before ResizeBuffers/shutdown, release only completed readbacks. A failed
// queue signal keeps its resources alive and refuses a potentially unsafe resize.
bool DrainScreenCapture(unsigned waitMilliseconds);
bool SupportsDisplayCaptureFormat(unsigned dxgiFormat);
// Format boundary shared with the isolated encoder tests. Pixels are top-down
// BGRA8; every successful file represents exactly width*height pixels.
bool SaveCaptureImage(const std::wstring& path, CaptureImageFormat format,
                      unsigned width, unsigned height, const std::vector<std::uint8_t>& bgra,
                      std::wstring& error);
const wchar_t* CaptureFormatExtension(CaptureImageFormat format);
}
