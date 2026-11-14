// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/controller_input.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
int failed = 0;
int checks = 0;
int queued = 0;
bool visible = false;
void Check(bool condition, const char* description) {
    ++checks;
    if (!condition) { ++failed; std::fprintf(stderr, "FAIL: %s\n", description); }
}
bool Only(const sm2debug::controller::InputBatch& batch, sm2debug::MenuInput expected) {
    return batch.count == 1 && batch.values[0] == expected;
}
std::vector<std::uint8_t> Report(std::size_t size, std::uint8_t id,
                                 std::size_t buttons, std::uint8_t b0 = 8,
                                 std::uint8_t b1 = 0, std::uint8_t b2 = 0) {
    std::vector<std::uint8_t> result(size);
    result[0] = id;
    result[buttons] = b0; result[buttons + 1] = b1; result[buttons + 2] = b2;
    return result;
}

void ParserTests() {
    using namespace sm2debug::controller;
    Check(IsControllerWindowClass(L"GameNxApp"), "game window class eligible");
    Check(!IsControllerWindowClass(nullptr), "null window class rejected");
    Check(!IsControllerWindowClass(L""), "empty window class rejected");
    Check(!IsControllerWindowClass(L"ConsoleWindowClass"), "unrelated window class rejected");
    Check(!IsControllerWindowClass(L"SM2OverlayPreviewOther"), "preview class prefix rejected");
#ifdef SM2DEBUG_PREVIEW
    Check(IsControllerWindowClass(L"SM2OverlayPreview"), "preview build accepts its own exact class");
#else
    Check(!IsControllerWindowClass(L"SM2OverlayPreview"), "production rejects preview window class");
#endif
    struct Format { std::uint16_t product; std::size_t size; std::size_t buttons; };
    constexpr std::array<Format, 10> formats{{
        {0x05c4,64,5}, {0x09cc,64,5}, {0x05c4,10,5}, {0x09cc,78,5},
        {0x0ce6,64,8}, {0x0df2,64,8}, {0x0ce6,10,5}, {0x0ce6,78,5},
        {0x0df2,10,5}, {0x0df2,78,5}
    }};
    std::uint32_t buttons = 999;
    for (const auto& format : formats) {
        auto data = Report(format.size, 1, format.buttons, 0x68, 3, 1);
        Check(ParseSonyReport(format.product, data.data(), data.size(), buttons) &&
            buttons == (Cross | Circle | L1 | R1 | PS), "USB/simple button layout");
        data[format.buttons + 2] = 0x02;
        Check(ParseSonyReport(format.product, data.data(), data.size(), buttons) &&
            !(buttons & PS), "touchpad press is not PS");
        data[format.buttons + 2] = 0x04;
        Check(ParseSonyReport(format.product, data.data(), data.size(), buttons) &&
            !(buttons & PS), "microphone/sequence bit is not PS");
    }
    // The expected CRC bytes were calculated independently with Python zlib
    // over 0xa1 + the 74 report bytes; this does not reuse the parser's CRC.
    auto ds4bt = Report(78, 0x11, 7, 0x68, 3, 1);
    ds4bt[1] = 0xc0;
    const std::array<std::uint8_t, 4> ds4crc{0x2e,0x89,0x56,0x2e};
    std::copy(ds4crc.begin(), ds4crc.end(), ds4bt.begin() + 74);
    Check(ParseSonyReport(0x05c4, ds4bt.data(), ds4bt.size(), buttons) &&
        buttons == (Cross | Circle | L1 | R1 | PS), "DS4 full Bluetooth golden CRC");
    Check(ParseSonyReport(0x09cc, ds4bt.data(), ds4bt.size(), buttons), "DS4 v2 Bluetooth");
    auto dualbt = Report(78, 0x31, 9, 0x68, 3, 1);
    dualbt[1] = 2;
    const std::array<std::uint8_t, 4> dualcrc{0xfb,0x7b,0xbd,0x7c};
    std::copy(dualcrc.begin(), dualcrc.end(), dualbt.begin() + 74);
    Check(ParseSonyReport(0x0ce6, dualbt.data(), dualbt.size(), buttons) &&
        buttons == (Cross | Circle | L1 | R1 | PS), "DualSense full Bluetooth golden CRC");
    Check(ParseSonyReport(0x0df2, dualbt.data(), dualbt.size(), buttons), "DualSense Edge Bluetooth");
    ds4bt[1] = 0x40;
    Check(!ParseSonyReport(0x05c4, ds4bt.data(), ds4bt.size(), buttons), "DS4 non-state BT flag refused");
    dualbt[11] ^= 1;
    Check(!ParseSonyReport(0x0ce6, dualbt.data(), dualbt.size(), buttons), "corrupt Bluetooth buttons rejected");
    dualbt[11] ^= 1;
    dualbt[77] ^= 0x80;
    Check(!ParseSonyReport(0x0ce6, dualbt.data(), dualbt.size(), buttons), "corrupt Bluetooth CRC rejected");
    constexpr std::array<std::uint32_t, 9> hats{
        Up, Up|Right, Right, Right|Down, Down, Down|Left, Left, Left|Up, 0};
    auto usb = Report(64, 1, 8);
    for (std::size_t hat = 0; hat < hats.size(); ++hat) {
        usb[8] = static_cast<std::uint8_t>(hat);
        Check(ParseSonyReport(0x0ce6, usb.data(), usb.size(), buttons) && buttons == hats[hat],
              "all D-pad directions and neutral");
    }
    for (int hat = 9; hat < 16; ++hat) {
        usb[8] = static_cast<std::uint8_t>(hat);
        Check(!ParseSonyReport(0x0ce6, usb.data(), usb.size(), buttons) && buttons == 0,
              "invalid D-pad hat rejected without buttons");
    }
    Check(!ParseSonyReport(0x0ce6, nullptr, 64, buttons), "null buffer rejected");
    Check(!ParseSonyReport(0xffff, usb.data(), usb.size(), buttons), "unsupported Sony PID rejected");
    for (std::size_t size = 0; size < 128; ++size) {
        auto data = Report(128, 1, 8);
        const bool parsed = ParseSonyReport(0x0ce6, data.data(), size, buttons);
        Check(parsed == (size == 10 || size == 64 || size == 78), "truncated/oversized report rejected");
    }
    usb[0] = 0x30;
    Check(!ParseSonyReport(0x0ce6, usb.data(), usb.size(), buttons), "unknown report ID rejected");
    // Every truncated BT length is rejected, including the byte before CRC end.
    for (std::size_t length = 0; length < 78; ++length)
        Check(!ParseSonyReport(0x0ce6, dualbt.data(), length, buttons), "truncated BT report rejected");
}

void TrackerTests() {
    using namespace sm2debug;
    using namespace sm2debug::controller;
    ButtonTracker tracker;
    Check(tracker.Feed(PS,0,true,false).count == 0, "held PS at connect does not toggle");
    Check(tracker.Feed(PS,1000,true,false).count == 0, "held PS does not repeat");
    Check(tracker.Feed(0,1010,true,false).count == 0, "PS release has no event");
    Check(Only(tracker.Feed(PS,1020,true,false),MenuInput::Toggle), "PS rising opens hidden menu");
    Check(tracker.Feed(PS,5000,true,true).count == 0, "long PS hold never repeats");
    tracker.Feed(0,5010,true,true);
    Check(Only(tracker.Feed(PS,5020,true,true),MenuInput::Toggle), "second PS edge closes visible menu");
    tracker.Feed(0,5030,true,true);
    Check(tracker.Feed(PS,5040,true,true).count == 0, "contact bounce does not double toggle");
    tracker.Feed(0,5300,true,false);
    Check(Only(tracker.Feed(PS,5310,true,false),MenuInput::Toggle), "PS edge after debounce accepted");
    Check(tracker.Feed(0,6000,false,true).count == 0, "unfocused controller ignored");
    Check(tracker.Feed(PS,6010,false,true).count == 0, "unfocused PS ignored");
    Check(tracker.Feed(PS,6020,true,true).count == 0, "PS held over focus gain ignored");
    tracker.Feed(0,6030,true,true);
    Check(Only(tracker.Feed(PS,6040,true,true),MenuInput::Toggle), "fresh focused PS press accepted");
    tracker.LoseFocus();
    Check(tracker.Feed(PS,7000,true,true).count == 0, "focus loss without reports still suppresses held PS");
    tracker.Reset();
    Check(tracker.Feed(PS,8000,true,true).count == 0, "disconnect/reconnect resets held PS baseline");
    tracker.Feed(0,8010,true,true);
    Check(Only(tracker.Feed(PS,8020,true,true),MenuInput::Toggle), "PS after reconnect release works");
    tracker.Reset();
    tracker.Feed(0,0,true,true);
    Check(Only(tracker.Feed(Down,10,true,true),MenuInput::Down), "D-pad edge navigates");
    Check(tracker.Feed(Down,359,true,true).count == 0, "D-pad initial repeat delay");
    Check(Only(tracker.Feed(Down,360,true,true),MenuInput::Down), "D-pad first repeat at delay");
    Check(tracker.Feed(Down,469,true,true).count == 0, "D-pad interval bounded");
    Check(Only(tracker.Feed(Down,470,true,true),MenuInput::Down), "D-pad second repeat");
    Check(Only(tracker.Feed(Down,99999,true,true),MenuInput::Down), "scheduling delay produces one repeat");
    tracker.Feed(0,100000,true,true);
    Check(Only(tracker.Feed(Left,100010,true,true),MenuInput::Left), "left adjusts");
    tracker.Feed(0,100020,true,true);
    Check(Only(tracker.Feed(Right,100030,true,true),MenuInput::Right), "right adjusts");
    tracker.Feed(0,100040,true,true);
    Check(Only(tracker.Feed(Up,100050,true,true),MenuInput::Up), "up navigates");
    tracker.Feed(0,100060,true,true);
    Check(Only(tracker.Feed(Cross,100070,true,true),MenuInput::Accept), "Cross accepts");
    Check(tracker.Feed(Cross,100500,true,true).count == 0, "Cross does not repeat");
    tracker.Feed(0,100510,true,true);
    Check(Only(tracker.Feed(Circle,100520,true,true),MenuInput::Back), "Circle goes back");
    tracker.Feed(0,100530,true,true);
    Check(Only(tracker.Feed(L1,100540,true,true),MenuInput::PreviousPage), "L1 previous page");
    tracker.Feed(0,100550,true,true);
    Check(Only(tracker.Feed(R1,100560,true,true),MenuInput::NextPage), "R1 next page");
    tracker.Feed(0,100570,true,false);
    Check(tracker.Feed(Cross|Circle|L1|R1|Up,100580,true,false).count == 0,
          "hidden menu suppresses navigation and actions");
    tracker.Feed(0,101000,true,true);
    Check(Only(tracker.Feed(PS|Cross|Down,101010,true,true),MenuInput::Toggle),
          "PS toggle takes precedence over same-packet actions");
    // Malformed reports are not fed to the tracker; a held PS remains held.
    const std::array<std::uint8_t,3> malformed{1,0,0};
    std::uint32_t decoded = 0;
    Check(!ParseSonyReport(0x0ce6,malformed.data(),malformed.size(),decoded), "malformed packet ignored");
    Check(tracker.Feed(PS,102000,true,true).count == 0, "malformed report cannot manufacture a PS edge");
}

void ReportSequenceTests() {
    using namespace sm2debug;
    using namespace sm2debug::controller;
    struct Sequence {
        std::uint16_t product;
        std::vector<std::uint8_t> neutral;
        std::vector<std::uint8_t> pressed;
    };
    std::vector<Sequence> sequences;
    sequences.push_back({0x05c4,Report(64,1,5),Report(64,1,5,8,0,1)});
    sequences.push_back({0x09cc,Report(10,1,5),Report(10,1,5,8,0,1)});
    sequences.push_back({0x0ce6,Report(64,1,8),Report(64,1,8,8,0,1)});
    sequences.push_back({0x0ce6,Report(10,1,5),Report(10,1,5,8,0,1)});
    sequences.push_back({0x0df2,Report(78,1,5),Report(78,1,5,8,0,1)});
    auto ds4neutral = Report(78,0x11,7);
    auto ds4pressed = Report(78,0x11,7,8,0,1);
    ds4neutral[1] = ds4pressed[1] = 0xc0;
    const std::array<std::uint8_t,4> ds4n{197,82,175,84}, ds4p{35,0,69,212};
    std::copy(ds4n.begin(),ds4n.end(),ds4neutral.begin()+74);
    std::copy(ds4p.begin(),ds4p.end(),ds4pressed.begin()+74);
    sequences.push_back({0x09cc,ds4neutral,ds4pressed});
    auto dualneutral = Report(78,0x31,9);
    auto dualpressed = Report(78,0x31,9,8,0,1);
    dualneutral[1] = dualpressed[1] = 2;
    const std::array<std::uint8_t,4> dualn{118,109,1,244}, dualp{170,7,178,164};
    std::copy(dualn.begin(),dualn.end(),dualneutral.begin()+74);
    std::copy(dualp.begin(),dualp.end(),dualpressed.begin()+74);
    sequences.push_back({0x0ce6,dualneutral,dualpressed});
    sequences.push_back({0x0df2,dualneutral,dualpressed});
    for (const auto& sequence : sequences) {
        ButtonTracker tracker;
        const auto feed = [&](const auto& report, std::uint64_t time, bool menuVisible) {
            std::uint32_t buttons = 0;
            Check(ParseSonyReport(sequence.product,report.data(),report.size(),buttons),
                  "PS sequence report decodes");
            return tracker.Feed(buttons,time,true,menuVisible);
        };
        Check(feed(sequence.neutral,0,false).count == 0, "report sequence connection seeds neutral");
        Check(Only(feed(sequence.pressed,10,false),MenuInput::Toggle), "report sequence PS press opens menu");
        Check(feed(sequence.pressed,1000,true).count == 0, "report sequence PS hold produces no toggle");
        Check(feed(sequence.neutral,1010,true).count == 0, "report sequence PS release produces no toggle");
        Check(Only(feed(sequence.pressed,1020,true),MenuInput::Toggle), "report sequence second PS closes menu");
    }
}

int Probe(unsigned durationMs) {
    const auto begin = GetTickCount64();
    std::uint64_t maxPollUs = 0;
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    while (GetTickCount64() - begin < durationMs) {
        LARGE_INTEGER start{}, finish{};
        QueryPerformanceCounter(&start);
        sm2debug::PollControllerInput(nullptr); // Always ineligible for menu input.
        QueryPerformanceCounter(&finish);
        const auto elapsed = static_cast<std::uint64_t>(
            (finish.QuadPart-start.QuadPart)*1000000/frequency.QuadPart);
        maxPollUs = (std::max)(maxPollUs,elapsed);
        Sleep(10);
    }
    const auto d = sm2debug::controller::GetDiagnostics();
    sm2debug::ShutdownControllerInput();
    const auto stopped = sm2debug::controller::GetDiagnostics();
    std::printf("{\"mode\":\"read_only_hid_probe\",\"duration_ms\":%u,"
        "\"enumerations\":%llu,\"sony_candidates\":%llu,\"opened_devices\":%llu,"
        "\"active_devices\":%zu,\"reports_parsed\":%llu,\"malformed_reports\":%llu,"
        "\"read_errors\":%llu,\"pending_reads\":%zu,\"max_poll_us\":%llu,"
        "\"queued_menu_actions\":%d,\"after_shutdown_active\":%zu,"
        "\"after_shutdown_retiring\":%zu,\"products\":[",
        durationMs,static_cast<unsigned long long>(d.enumerationRuns),
        static_cast<unsigned long long>(d.sonyCandidates),
        static_cast<unsigned long long>(d.openedDevices),d.activeDevices,
        static_cast<unsigned long long>(d.reportsParsed),
        static_cast<unsigned long long>(d.malformedReports),
        static_cast<unsigned long long>(d.readErrors),d.pendingReads,
        static_cast<unsigned long long>(maxPollUs),queued,stopped.activeDevices,
        stopped.retiringDevices);
    for (std::size_t i = 0; i < d.activeDevices; ++i)
        std::printf("%s\"%04x\"",i?",":"",d.products[i]);
    std::puts("]}");
    return queued == 0 && stopped.activeDevices == 0 ? 0 : 1;
}
}

namespace sm2debug {
void QueueMenuInput(MenuInput) { ++queued; }
bool MenuVisible() { return visible; }
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1],"--probe") == 0) {
        const unsigned requested = argc > 2 ? static_cast<unsigned>(std::strtoul(argv[2],nullptr,10)) : 3100;
        return Probe((std::min)(requested,10000u));
    }
    ParserTests();
    TrackerTests();
    ReportSequenceTests();
    std::printf("{\"mode\":\"deterministic_tests\",\"checks\":%d,\"failed\":%d,\"passed\":%s}\n",
                checks,failed,failed==0?"true":"false");
    return failed == 0 ? 0 : 1;
}
