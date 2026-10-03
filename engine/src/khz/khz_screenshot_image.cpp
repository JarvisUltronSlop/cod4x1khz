#include "khz_screenshot_image.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <wincodec.h>
#include <algorithm>
#include <cstring>
#pragma comment(lib,"windowscodecs.lib")
#pragma comment(lib,"ole32.lib")
#pragma comment(lib,"oleaut32.lib")

namespace khz::screenshot {
namespace {
template<class T> struct Object {T* pointer=nullptr;~Object(){if(pointer)pointer->Release();}T* operator->()const{return pointer;}T** Out(){return &pointer;}};
struct Apartment {HRESULT result=CoInitializeEx(nullptr,COINIT_MULTITHREADED);~Apartment(){if(SUCCEEDED(result))CoUninitialize();}};
bool factory(Object<IWICImagingFactory>* output){return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(output->Out())));}
bool jpeg(const std::uint8_t* bytes,unsigned length){return bytes&&length>=4&&length<=MaximumBytes&&bytes[0]==255&&bytes[1]==216&&bytes[length-2]==255&&bytes[length-1]==217;}
}
bool EncodeJpeg(const std::uint8_t* bgr,unsigned width,unsigned height,std::vector<std::uint8_t>* output,unsigned* encodedWidth,unsigned* encodedHeight){
    if(!bgr||!output||!encodedWidth||!encodedHeight||!width||width>MaximumCaptureWidth||!height||height>MaximumCaptureHeight)return false;
    Apartment apartment;if(FAILED(apartment.result))return false;Object<IWICImagingFactory> imaging;if(!factory(&imaging))return false;
    Object<IWICBitmap> bitmap;
    if(FAILED(imaging->CreateBitmapFromMemory(width,height,GUID_WICPixelFormat24bppBGR,width*3,width*height*3,const_cast<BYTE*>(bgr),bitmap.Out())))return false;
    const double scale=(std::min)({1.0,double(MaximumWidth)/width,double(MaximumHeight)/height});
    const unsigned targetWidth=(std::max)(1u,unsigned(width*scale)),targetHeight=(std::max)(1u,unsigned(height*scale));
    Object<IWICBitmapScaler> scaler;IWICBitmapSource* source=bitmap.pointer;
    if(targetWidth!=width||targetHeight!=height){if(FAILED(imaging->CreateBitmapScaler(scaler.Out()))||FAILED(scaler->Initialize(source,targetWidth,targetHeight,WICBitmapInterpolationModeFant)))return false;source=scaler.pointer;}
    Object<IStream> stream;if(FAILED(CreateStreamOnHGlobal(nullptr,TRUE,stream.Out())))return false;
    Object<IWICBitmapEncoder> encoder;Object<IWICBitmapFrameEncode> frame;Object<IPropertyBag2> properties;
    if(FAILED(imaging->CreateEncoder(GUID_ContainerFormatJpeg,nullptr,encoder.Out()))||FAILED(encoder->Initialize(stream.pointer,WICBitmapEncoderNoCache))
        ||FAILED(encoder->CreateNewFrame(frame.Out(),properties.Out())))return false;
    PROPBAG2 quality{};quality.pstrName=const_cast<wchar_t*>(L"ImageQuality");VARIANT value{};value.vt=VT_R4;value.fltVal=.8f;
    if(FAILED(properties->Write(1,&quality,&value))||FAILED(frame->Initialize(properties.pointer))||FAILED(frame->SetSize(targetWidth,targetHeight)))return false;
    WICPixelFormatGUID format=GUID_WICPixelFormat24bppBGR;if(FAILED(frame->SetPixelFormat(&format)))return false;
    Object<IWICFormatConverter> converter;
    if(format!=GUID_WICPixelFormat24bppBGR){if(FAILED(imaging->CreateFormatConverter(converter.Out()))||FAILED(converter->Initialize(source,format,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)))return false;source=converter.pointer;}
    if(FAILED(frame->WriteSource(source,nullptr))||FAILED(frame->Commit())||FAILED(encoder->Commit()))return false;
    STATSTG stat{};if(FAILED(stream->Stat(&stat,STATFLAG_NONAME))||stat.cbSize.QuadPart<4||stat.cbSize.QuadPart>MaximumBytes)return false;
    LARGE_INTEGER start{};if(FAILED(stream->Seek(start,STREAM_SEEK_SET,nullptr)))return false;
    output->resize(std::size_t(stat.cbSize.QuadPart));ULONG read=0;
    if(FAILED(stream->Read(output->data(),ULONG(output->size()),&read))||read!=output->size()||!jpeg(output->data(),unsigned(output->size()))){output->clear();return false;}
    *encodedWidth=targetWidth;*encodedHeight=targetHeight;return true;
}
bool ValidateJpeg(const std::uint8_t* bytes,unsigned length,unsigned width,unsigned height){
    if(!jpeg(bytes,length)||!width||width>MaximumWidth||!height||height>MaximumHeight)return false;
    Apartment apartment;if(FAILED(apartment.result))return false;Object<IWICImagingFactory> imaging;if(!factory(&imaging))return false;
    Object<IWICStream> stream;Object<IWICBitmapDecoder> decoder;Object<IWICBitmapFrameDecode> frame;
    if(FAILED(imaging->CreateStream(stream.Out()))||FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes),length))
        ||FAILED(imaging->CreateDecoderFromStream(stream.pointer,nullptr,WICDecodeMetadataCacheOnDemand,decoder.Out())))return false;
    UINT count=0,actualWidth=0,actualHeight=0;
    if(FAILED(decoder->GetFrameCount(&count))||count!=1||FAILED(decoder->GetFrame(0,frame.Out()))||FAILED(frame->GetSize(&actualWidth,&actualHeight))||actualWidth!=width||actualHeight!=height)return false;
    Object<IWICFormatConverter> converter;
    if(FAILED(imaging->CreateFormatConverter(converter.Out()))||FAILED(converter->Initialize(frame.pointer,GUID_WICPixelFormat24bppBGR,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)))return false;
    std::vector<std::uint8_t> pixels(std::size_t(width)*height*3);
    return SUCCEEDED(converter->CopyPixels(nullptr,width*3,UINT(pixels.size()),pixels.data()));
}
}
