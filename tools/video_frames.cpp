// Small local reference-video decoder using the Windows Media Foundation runtime.
// This utility only reads the supplied video and writes BMP frames to the given folder.
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <vector>

template<class T> void Release(T*& value) { if (value) { value->Release(); value = nullptr; } }

bool SaveFrame(IMFSample* sample, const std::filesystem::path& path, UINT32 width, UINT32 height, LONG defaultStride) {
    IMFMediaBuffer* buffer = nullptr;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) return false;
    IMF2DBuffer* twoDimensional = nullptr;
    BYTE* data = nullptr;
    LONG stride = defaultStride;
    DWORD length = 0;
    bool locked2D = SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&twoDimensional)));
    HRESULT result = locked2D ? twoDimensional->Lock2D(&data, &stride) : buffer->Lock(&data, nullptr, &length);
    if (FAILED(result)) { Release(twoDimensional); Release(buffer); return false; }
    // Some software converters report an unpadded default stride while returning
    // complete aligned RGB rows from the contiguous buffer.
    if (!locked2D && height && length % height == 0 && length / height >= width * 4 &&
        length / height <= width * 4 + 256)
        stride = static_cast<LONG>(length / height) * (defaultStride < 0 ? -1 : 1);
    if (!locked2D && stride < 0) data += static_cast<size_t>(height - 1) * static_cast<size_t>(-stride);
    const DWORD imageBytes = width * height * 4;
    BITMAPFILEHEADER file{};
    file.bfType = 0x4d42;
    file.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    file.bfSize = file.bfOffBits + imageBytes;
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = static_cast<LONG>(width);
    info.biHeight = -static_cast<LONG>(height);
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    info.biSizeImage = imageBytes;
    FILE* output = _wfopen(path.c_str(), L"wb");
    bool saved = false;
    if (output) {
        saved = fwrite(&file, sizeof(file), 1, output) == 1 && fwrite(&info, sizeof(info), 1, output) == 1;
        for (UINT32 row = 0; saved && row < height; ++row)
            saved = fwrite(data + static_cast<ptrdiff_t>(row) * stride, width * 4, 1, output) == 1;
        fclose(output);
    }
    if (locked2D) twoDimensional->Unlock2D(); else buffer->Unlock();
    Release(twoDimensional);
    Release(buffer);
    return saved;
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) { fwprintf(stderr, L"Usage: video_frames video.mp4 output-folder\n"); return 2; }
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(result)) return 3;
    result = MFStartup(MF_VERSION);
    if (FAILED(result)) { CoUninitialize(); return 4; }
    IMFAttributes* attributes = nullptr;
    IMFSourceReader* reader = nullptr;
    IMFMediaType* mediaType = nullptr;
    MFCreateAttributes(&attributes, 2);
    attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    attributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, FALSE);
    result = MFCreateSourceReaderFromURL(argv[1], attributes, &reader);
    Release(attributes);
    if (FAILED(result)) { fwprintf(stderr, L"Open failed 0x%08lx\n", static_cast<unsigned long>(result)); MFShutdown(); CoUninitialize(); return 5; }
    reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    MFCreateMediaType(&mediaType);
    mediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    mediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    result = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, mediaType);
    Release(mediaType);
    if (FAILED(result)) { fwprintf(stderr, L"RGB32 failed 0x%08lx\n", static_cast<unsigned long>(result)); Release(reader); MFShutdown(); CoUninitialize(); return 6; }
    reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &mediaType);
    UINT32 width = 0, height = 0, strideValue = 0;
    MFGetAttributeSize(mediaType, MF_MT_FRAME_SIZE, &width, &height);
    if (FAILED(mediaType->GetUINT32(MF_MT_DEFAULT_STRIDE, &strideValue))) strideValue = width * 4;
    Release(mediaType);
    PROPVARIANT duration{};
    reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &duration);
    LONGLONG length = duration.vt == VT_UI8 ? static_cast<LONGLONG>(duration.uhVal.QuadPart) : 0;
    fwprintf(stdout, L"Video %ux%u duration %.3f seconds\n", width, height, length / 10000000.0);
    PropVariantClear(&duration);
    std::filesystem::create_directories(argv[2]);
    const LONGLONG step = length > 0 ? std::max<LONGLONG>(10000000, length / 15) : 20000000;
    LONGLONG next = 0;
    unsigned frames = 0;
    while (frames < 16) {
        DWORD flags = 0;
        LONGLONG time = 0;
        IMFSample* sample = nullptr;
        result = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &time, &sample);
        if (FAILED(result) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) { Release(sample); break; }
        if (sample && time >= next) {
            wchar_t filename[80]{};
            swprintf(filename, std::size(filename), L"frame-%02u-%.2fs.bmp", frames, time / 10000000.0);
            if (SaveFrame(sample, std::filesystem::path(argv[2]) / filename, width, height, static_cast<LONG>(strideValue))) {
                fwprintf(stdout, L"%ls\n", filename);
                ++frames;
            }
            next += step;
        }
        Release(sample);
    }
    Release(reader);
    MFShutdown();
    CoUninitialize();
    return frames > 0 ? 0 : 7;
}
