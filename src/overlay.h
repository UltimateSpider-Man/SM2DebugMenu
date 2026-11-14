#pragma once
#include <windows.h>

namespace sm2debug {
enum class MenuInput { Toggle, Up, Down, Left, Right, Accept, Back, PreviousPage, NextPage, Close };

// Producers may run on the controller, window or render thread.
void QueueMenuInput(MenuInput input);
bool MenuVisible();
// Called on the render thread, before opening an ImGui frame.
bool PumpMenuInput();
// Called inside an active ImGui frame. Never accesses engine objects directly.
void RenderMenuFrame();

// The production renderer attaches to the existing game swapchain.
bool InitializeOverlay(HMODULE module);
void ShutdownOverlay();
int RunOverlayPreview(HINSTANCE instance);

// Read-only HID input; no controller output reports or game input hooks.
void PollControllerInput(HWND gameWindow);
void ShutdownControllerInput();
}
