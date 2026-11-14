// SPDX-License-Identifier: GPL-3.0-or-later
#include "screen_capture.h"
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wincodec.h>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace sm2debug {
namespace {
template<class T> struct CaptureCom {
    T* pointer = nullptr;
    ~CaptureCom() { if (pointer) pointer->Release(); }
    T** put() { return &pointer; }
    T* operator->() const { return pointer; }
};
void CheckCapture(HRESULT code, const char* message) { if (FAILED(code)) throw std::runtime_error(message); }
void RequireCapture(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
bool ValidImage(unsigned width, unsigned height, std::size_t bytes) {
    return width && height && width <= 16384 && height <= 16384 &&
        std::uint64_t(width) * height <= 64u * 1024u * 1024u &&
        std::uint64_t(width) * height * 4 == bytes;
}
struct Readback {
    CaptureCom<ID3D12Resource> source, buffer;
    CaptureCom<ID3D12CommandAllocator> allocator;
    CaptureCom<ID3D12GraphicsCommandList> commands;
    CaptureCom<ID3D12Fence> fence;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes = 0;
    DXGI_FORMAT pixelFormat = DXGI_FORMAT_UNKNOWN;
    CaptureImageFormat imageFormat = CaptureImageFormat::Bmp;
    std::wstring directory;
    bool submitted = false, tracked = false;
    HANDLE event = nullptr;
    bool eventRegistered = false;
    ~Readback() { if (event) CloseHandle(event); }
};
struct CaptureService {
    std::mutex mutex;
    ScreenCaptureState state;
    bool queued = false;
    CaptureImageFormat requestedFormat = CaptureImageFormat::Bmp;
    std::unique_ptr<Readback> readback;
};
// Callback module is pinned. Failed/untracked GPU work must retain its resources
// until process teardown, never a static destructor on DLL shutdown.
CaptureService& Service() { static auto* service = new CaptureService; return *service; }

std::wstring NewCapturePath(const std::wstring& directory, CaptureImageFormat format) {
    namespace fs = std::filesystem;
    const auto folder = fs::path(directory) / L"captures";
    fs::create_directories(folder);
    SYSTEMTIME time{}; GetLocalTime(&time);
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        wchar_t name[160]{};
        swprintf(name, std::size(name), L"capture-%04u%02u%02u-%02u%02u%02u-%03u-%llu-%u.%ls",
            time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond,time.wMilliseconds,
            static_cast<unsigned long long>(GetTickCount64()),attempt,CaptureFormatExtension(format));
        auto path = (folder / name).wstring();
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) { CloseHandle(file); return path; }
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS)
            throw std::runtime_error("Cannot create a capture file");
    }
    throw std::runtime_error("Cannot allocate a unique capture filename");
}

bool FinishReadback(CaptureService& service) {
    auto* readback = service.readback.get();
    if (!readback) return true;
    if (!readback->tracked) return false;
    const auto completed = readback->fence->GetCompletedValue();
    if (completed == std::numeric_limits<UINT64>::max()) {
        service.state.ready = false;
        service.state.status = L"Capture failed: graphics device removed; resources retained";
        return false;
    }
    if (completed < 1) return false;
    try {
        const auto width = readback->footprint.Footprint.Width, height = readback->footprint.Footprint.Height;
        std::vector<std::uint8_t> pixels(std::size_t(width) * height * 4);
        std::uint8_t* mapped = nullptr;
        D3D12_RANGE range{0, static_cast<SIZE_T>(readback->bytes)};
        CheckCapture(readback->buffer->Map(0, &range, reinterpret_cast<void**>(&mapped)), "Cannot map captured pixels");
        for (unsigned y=0; y<height; ++y) {
            const auto* source = mapped + readback->footprint.Offset + std::size_t(y) * readback->footprint.Footprint.RowPitch;
            auto* destination = pixels.data() + std::size_t(y) * width * 4;
            std::memcpy(destination, source, std::size_t(width) * 4);
            for (unsigned x=0; x<width; ++x) {
                if (readback->pixelFormat == DXGI_FORMAT_R8G8B8A8_UNORM || readback->pixelFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
                    std::swap(destination[x*4], destination[x*4+2]);
                destination[x*4+3] = 255;
            }
        }
        const D3D12_RANGE written{0,0}; readback->buffer->Unmap(0, &written);
        const auto path = NewCapturePath(readback->directory, readback->imageFormat);
        std::wstring error;
        if (!SaveCaptureImage(path, readback->imageFormat, width, height, pixels, error)) {
            DeleteFileW(path.c_str()); // Only this request's newly created file.
            service.state.status = L"Capture failed: " + error;
        } else {
            ++service.state.completed;
            service.state.status = L"Saved " + path;
            service.state.lastPath = path;
        }
    } catch (...) { service.state.status = L"Capture failed while reading or saving the completed image"; }
    service.readback.reset(); service.state.busy = service.queued;
    return true;
}
}

const wchar_t* CaptureFormatExtension(CaptureImageFormat format) {
    switch(format) {
    case CaptureImageFormat::Bmp:return L"bmp";
    case CaptureImageFormat::Dds:return L"dds";
    case CaptureImageFormat::Jpg:return L"jpg";
    case CaptureImageFormat::Png:return L"png";
    }
    return L"";
}
bool SaveCaptureImage(const std::wstring& path, CaptureImageFormat format, unsigned width, unsigned height,
                      const std::vector<std::uint8_t>& pixels, std::wstring& error) {
    error.clear();
    if (!ValidImage(width,height,pixels.size()) || *CaptureFormatExtension(format)==0) {
        error=L"Invalid capture dimensions, pixels or format"; return false;
    }
    if (format == CaptureImageFormat::Dds) {
        // DDS legacy A8R8G8B8 header; masks describe our BGRA byte order.
        std::array<std::uint32_t,32> header{};
        header[0]=0x20534444; header[1]=124; header[2]=0x100f;
        header[3]=height; header[4]=width; header[5]=width*4;
        header[19]=32; header[20]=0x41; header[22]=32;
        header[23]=0x00ff0000; header[24]=0x0000ff00; header[25]=0x000000ff; header[26]=0xff000000;
        header[27]=0x1000;
        std::ofstream file(std::filesystem::path(path),std::ios::binary|std::ios::trunc);
        file.write(reinterpret_cast<const char*>(header.data()),sizeof(header));
        file.write(reinterpret_cast<const char*>(pixels.data()),static_cast<std::streamsize>(pixels.size()));
        file.flush();
        if (!file) { error=L"DDS write failed"; return false; }
        return true;
    }
    const HRESULT com = CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    if (FAILED(com) && com != RPC_E_CHANGED_MODE) { error=L"Image encoder COM initialization failed"; return false; }
    bool success = false;
    try {
        CaptureCom<IWICImagingFactory> factory;
        CheckCapture(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,
            IID_IWICImagingFactory,reinterpret_cast<void**>(factory.put())),"Cannot create WIC encoder");
        CaptureCom<IWICStream> stream;
        CheckCapture(factory->CreateStream(stream.put()),"Cannot create capture stream");
        CheckCapture(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE),"Cannot open capture output");
        const auto& container = format==CaptureImageFormat::Bmp ? GUID_ContainerFormatBmp :
            (format==CaptureImageFormat::Jpg ? GUID_ContainerFormatJpeg : GUID_ContainerFormatPng);
        CaptureCom<IWICBitmapEncoder> encoder;
        CheckCapture(factory->CreateEncoder(container,nullptr,encoder.put()),"Capture codec unavailable");
        CheckCapture(encoder->Initialize(stream.pointer,WICBitmapEncoderNoCache),"Cannot initialize capture codec");
        CaptureCom<IWICBitmapFrameEncode> frame; CaptureCom<IPropertyBag2> properties;
        CheckCapture(encoder->CreateNewFrame(frame.put(),properties.put()),"Cannot create encoded frame");
        CheckCapture(frame->Initialize(properties.pointer),"Cannot initialize encoded frame");
        CheckCapture(frame->SetSize(width,height),"Cannot set image dimensions");
        WICPixelFormatGUID pixelFormat = format==CaptureImageFormat::Png ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR;
        CheckCapture(frame->SetPixelFormat(&pixelFormat),"Cannot select image pixel format");
        CaptureCom<IWICBitmap> bitmap;
        CheckCapture(factory->CreateBitmapFromMemory(width,height,GUID_WICPixelFormat32bppBGRA,width*4,
            static_cast<UINT>(pixels.size()),const_cast<BYTE*>(pixels.data()),bitmap.put()),"Cannot create image pixels");
        CaptureCom<IWICFormatConverter> converter;
        CheckCapture(factory->CreateFormatConverter(converter.put()),"Cannot create pixel converter");
        CheckCapture(converter->Initialize(bitmap.pointer,pixelFormat,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom),"Cannot convert image pixels");
        CheckCapture(frame->WriteSource(converter.pointer,nullptr),"Cannot write captured pixels");
        CheckCapture(frame->Commit(),"Cannot commit captured frame");
        CheckCapture(encoder->Commit(),"Cannot commit capture file");
        success=true;
    } catch (...) { error=L"Windows image codec could not save the capture"; }
    if (SUCCEEDED(com)) CoUninitialize();
    return success;
}

bool SupportsDisplayCaptureFormat(unsigned value) {
    switch(static_cast<DXGI_FORMAT>(value)) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:return true;
    default:return false;
    }
}
ScreenCaptureState ReadScreenCaptureState() {
    auto& service=Service();std::lock_guard lock(service.mutex);return service.state;
}
void SetScreenCaptureAvailable(bool available,const std::wstring& reason) {
    auto& service=Service();std::lock_guard lock(service.mutex);
    service.state.ready=available;
    if (!available) {service.queued=false;service.state.busy=bool(service.readback);service.state.status=reason;}
    else if (!service.state.busy && !service.state.completed) service.state.status=L"Ready to capture the display with its HUD";
}
bool CycleCaptureImageFormat(int direction) {
    auto& service=Service();std::lock_guard lock(service.mutex);
    if(service.state.busy)return false;
    auto value=static_cast<unsigned>(service.state.format);
    service.state.format=static_cast<CaptureImageFormat>((value+(direction<0?3:1))%4);
    return true;
}
bool RequestDisplayCapture() {
    auto& service=Service();std::lock_guard lock(service.mutex);
    if(!service.state.ready||service.state.busy)return false;
    service.requestedFormat=service.state.format;service.queued=true;service.state.busy=true;
    service.state.status=L"Capture queued; waiting for the debug menu to close";return true;
}
void ProcessScreenCapture(ID3D12Device* device,ID3D12CommandQueue* queue,IDXGISwapChain3* swap,
                          const std::wstring& directory,bool menuVisible) {
    auto& service=Service();std::lock_guard lock(service.mutex);
    if(!FinishReadback(service)||!service.queued||menuVisible)return;
    service.queued=false;
    try {
        RequireCapture(service.state.ready&&device&&queue&&swap,"Capture display unavailable");
        auto job=std::make_unique<Readback>();
        job->imageFormat=service.requestedFormat;job->directory=directory;
        CheckCapture(swap->GetBuffer(swap->GetCurrentBackBufferIndex(),__uuidof(ID3D12Resource),
            reinterpret_cast<void**>(job->source.put())),"Cannot acquire display buffer");
        const auto source=job->source->GetDesc();job->pixelFormat=source.Format;
        RequireCapture(source.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&source.DepthOrArraySize==1&&
            source.MipLevels==1&&source.SampleDesc.Count==1&&SupportsDisplayCaptureFormat(source.Format)&&
            source.Width<=16384&&ValidImage(static_cast<unsigned>(source.Width),source.Height,source.Width*source.Height*4),
            "Unsupported display capture dimensions or pixel format");
        UINT rows=0;UINT64 rowBytes=0;
        device->GetCopyableFootprints(&source,0,1,0,&job->footprint,&rows,&rowBytes,&job->bytes);
        RequireCapture(job->footprint.Footprint.Width==source.Width&&job->footprint.Footprint.Height==source.Height&&
            job->footprint.Footprint.Depth==1&&job->footprint.Footprint.Format==source.Format&&
            rows==source.Height&&rowBytes==source.Width*4&&job->footprint.Footprint.RowPitch>=rowBytes&&
            rows && job->footprint.Offset+UINT64(rows-1)*job->footprint.Footprint.RowPitch+rowBytes<=job->bytes&&job->bytes<=512ull*1024*1024,
            "Invalid display copy footprint");
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=job->bytes;
        buffer.Height=1;buffer.DepthOrArraySize=1;buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        CheckCapture(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,__uuidof(ID3D12Resource),reinterpret_cast<void**>(job->buffer.put())),"Cannot create readback buffer");
        CheckCapture(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,__uuidof(ID3D12CommandAllocator),
            reinterpret_cast<void**>(job->allocator.put())),"Cannot create capture allocator");
        CheckCapture(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,job->allocator.pointer,nullptr,
            __uuidof(ID3D12GraphicsCommandList),reinterpret_cast<void**>(job->commands.put())),"Cannot create capture commands");
        CheckCapture(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,__uuidof(ID3D12Fence),
            reinterpret_cast<void**>(job->fence.put())),"Cannot create capture fence");
        job->event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        RequireCapture(job->event!=nullptr,"Cannot create capture completion event");
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource=job->source.pointer;barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_PRESENT;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
        job->commands->ResourceBarrier(1,&barrier);
        D3D12_TEXTURE_COPY_LOCATION from{};from.pResource=job->source.pointer;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION to{};to.pResource=job->buffer.pointer;to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=job->footprint;
        job->commands->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);job->commands->ResourceBarrier(1,&barrier);
        CheckCapture(job->commands->Close(),"Cannot close capture commands");
        ID3D12CommandList* lists[]{job->commands.pointer};queue->ExecuteCommandLists(1,lists);job->submitted=true;
        service.readback=std::move(job);
        CheckCapture(queue->Signal(service.readback->fence.pointer,1),"Cannot track capture completion");
        service.readback->tracked=true;service.state.status=L"Reading the captured display";
    } catch (...) {
        service.state.busy=bool(service.readback);
        if(service.readback){service.state.ready=false;service.state.status=L"Capture queue failed; GPU resources retained";}
        else service.state.status=L"Capture failed before GPU submission";
    }
}
bool DrainScreenCapture(unsigned waitMilliseconds) {
    auto& service=Service();std::lock_guard lock(service.mutex);
    if(FinishReadback(service))return true;
    if(!service.readback->tracked)return false;
    auto& job=*service.readback;
    if(!job.eventRegistered) {
        if(FAILED(job.fence->SetEventOnCompletion(1,job.event)))return false;
        job.eventRegistered=true;
    }
    const bool completed=WaitForSingleObject(job.event,waitMilliseconds)==WAIT_OBJECT_0;
    return completed&&FinishReadback(service);
}
}
