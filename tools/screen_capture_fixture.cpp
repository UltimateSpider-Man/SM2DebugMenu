// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/screen_capture.h"
#include <windows.h>
#include <wincodec.h>
#include <dxgiformat.h>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
using namespace sm2debug;
static unsigned checks=0;
static void Expect(bool condition,const char* reason){++checks;if(!condition)throw std::runtime_error(reason);}
template<class T>struct Com{T* p=nullptr;~Com(){if(p)p->Release();}T** put(){return &p;}T* operator->()const{return p;}};
static std::vector<std::uint8_t> Decode(const std::filesystem::path& path,unsigned width,unsigned height){
    Com<IWICImagingFactory> factory;Com<IWICBitmapDecoder> decoder;Com<IWICBitmapFrameDecode> frame;Com<IWICFormatConverter> converter;
    Expect(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_IWICImagingFactory,reinterpret_cast<void**>(factory.put()))),"decoder factory");
    Expect(SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,decoder.put())),"decode saved file");
    Expect(SUCCEEDED(decoder->GetFrame(0,frame.put())),"decode first frame");
    UINT w=0,h=0;Expect(SUCCEEDED(frame->GetSize(&w,&h))&&w==width&&h==height,"exact saved dimensions");
    Expect(SUCCEEDED(factory->CreateFormatConverter(converter.put())),"decoder converter");
    Expect(SUCCEEDED(converter->Initialize(frame.p,GUID_WICPixelFormat32bppBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)),"decode BGRA");
    std::vector<std::uint8_t> result(width*height*4);
    Expect(SUCCEEDED(converter->CopyPixels(nullptr,width*4,static_cast<UINT>(result.size()),result.data())),"read decoded pixels");return result;
}
int wmain(int argc,wchar_t** argv){
    try{
        Expect(argc==2,"isolated output directory required");
        const std::filesystem::path directory(argv[1]);std::filesystem::create_directories(directory);
        Expect(SUCCEEDED(CoInitializeEx(nullptr,COINIT_MULTITHREADED)),"fixture COM");
        constexpr unsigned width=19,height=11;
        std::vector<std::uint8_t> pixels(width*height*4);
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){const auto i=(y*width+x)*4;
            pixels[i]=static_cast<std::uint8_t>(30+x*4);pixels[i+1]=static_cast<std::uint8_t>(50+y*8);
            pixels[i+2]=static_cast<std::uint8_t>(190-x*3);pixels[i+3]=255;}
        std::wstring error;
        for(auto format:{CaptureImageFormat::Bmp,CaptureImageFormat::Dds,CaptureImageFormat::Jpg,CaptureImageFormat::Png}){
            const auto path=directory/(std::wstring(L"fixture.")+CaptureFormatExtension(format));
            Expect(SaveCaptureImage(path.wstring(),format,width,height,pixels,error)&&error.empty(),"encode image");
            if(format==CaptureImageFormat::Dds){
                std::ifstream stream(path,std::ios::binary);std::vector<char> bytes((std::istreambuf_iterator<char>(stream)),{});
                Expect(bytes.size()==128+pixels.size(),"DDS exact length");
                std::array<std::uint32_t,32> header{};std::memcpy(header.data(),bytes.data(),128);
                Expect(header[0]==0x20534444&&header[1]==124&&header[19]==32,"DDS magic/header sizes");
                Expect(header[3]==height&&header[4]==width&&header[5]==width*4,"DDS dimensions/stride");
                Expect(header[20]==0x41&&header[22]==32&&header[23]==0xff0000&&header[24]==0xff00&&header[25]==0xff&&header[26]==0xff000000,"DDS actual BGRA masks");
                Expect(std::memcmp(bytes.data()+128,pixels.data(),pixels.size())==0,"DDS payload exact");
            }else{
                const auto decoded=Decode(path,width,height);
                for(std::size_t i=0;i<pixels.size();++i){
                    if(format==CaptureImageFormat::Jpg&&i%4!=3)Expect(std::abs(int(decoded[i])-int(pixels[i]))<=18,"JPEG colors bounded");
                    else Expect(decoded[i]==pixels[i],"lossless colors/order/alpha");
                }
            }
        }
        const auto invalid=directory/L"must-not-exist.png";
        Expect(!SaveCaptureImage(invalid.wstring(),CaptureImageFormat::Png,0,height,pixels,error),"reject zero width");
        Expect(!SaveCaptureImage(invalid.wstring(),CaptureImageFormat::Png,width,height+1,pixels,error),"reject short input");
        Expect(!SaveCaptureImage(invalid.wstring(),static_cast<CaptureImageFormat>(100),width,height,pixels,error),"reject bad format");
        Expect(!SaveCaptureImage(invalid.wstring(),CaptureImageFormat::Png,UINT_MAX,UINT_MAX,pixels,error),"reject overflow");
        Expect(!std::filesystem::exists(invalid),"invalid requests never create files");
        Expect(!SaveCaptureImage((directory/L"absent"/L"bad.png").wstring(),CaptureImageFormat::Png,width,height,pixels,error),"encode failure reported");
        for(auto format:{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,DXGI_FORMAT_B8G8R8X8_UNORM,DXGI_FORMAT_B8G8R8X8_UNORM_SRGB})Expect(SupportsDisplayCaptureFormat(format),"supported display format");
        Expect(!SupportsDisplayCaptureFormat(DXGI_FORMAT_R16G16B16A16_FLOAT),"do not mislabel pre-tonemap HDR");
        Expect(!SupportsDisplayCaptureFormat(DXGI_FORMAT_R10G10B10A2_UNORM),"do not reinterpret10bit colors");
        Expect(!RequestDisplayCapture(),"cannot queue before display ready");
        Expect(ReadScreenCaptureState().format==CaptureImageFormat::Bmp,"source enum initial index");
        Expect(CycleCaptureImageFormat(-1)&&ReadScreenCaptureState().format==CaptureImageFormat::Png,"format wraps left");
        Expect(CycleCaptureImageFormat(1)&&ReadScreenCaptureState().format==CaptureImageFormat::Bmp,"format wraps right");
        SetScreenCaptureAvailable(true,L"");Expect(RequestDisplayCapture(),"queue ready display");
        Expect(ReadScreenCaptureState().busy&&!RequestDisplayCapture()&&!CycleCaptureImageFormat(1),"one request captures immutable format");
        ProcessScreenCapture(nullptr,nullptr,nullptr,directory.wstring(),true);
        Expect(ReadScreenCaptureState().busy&&ReadScreenCaptureState().completed==0,"visible menu delays GPU access");
        SetScreenCaptureAvailable(false,L"Fixture shutdown");Expect(!ReadScreenCaptureState().busy&&!ReadScreenCaptureState().ready,"shutdown cancels unsubmitted work");
        Expect(DrainScreenCapture(0),"empty drain");
        CoUninitialize();
        std::cout<<"{\"passed\":true,\"assertions\":"<<checks<<",\"formats\":4,\"actual_windows_codecs\":true,\"game_started\":false,\"gpu_used\":false}\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<" after "<<checks<<" checks\n";return 1;}
}
