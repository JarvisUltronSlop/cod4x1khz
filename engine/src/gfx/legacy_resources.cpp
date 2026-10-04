#include "legacy_resources.h"
#include "legacy_factory.h"
#include <algorithm>
#include <string>

namespace gfx::legacy {
namespace {thread_local std::string error;}
HRESULT unsupported(const char* method) {error="Native Vulkan graphics failure: "+std::string(method);OutputDebugStringA((error+"\n").c_str());return E_NOTIMPL;}
const char* lastError(){return error.c_str();}
ImageFormat imageFormat(D3DFORMAT f) {
    switch(f){
    case D3DFMT_A8R8G8B8:case D3DFMT_X8R8G8B8:return ImageFormat::Bgra8;
    case D3DFMT_A8B8G8R8:case D3DFMT_X8B8G8R8:return ImageFormat::Rgba8;
    case D3DFMT_DXT1:return ImageFormat::Bc1;case D3DFMT_DXT3:return ImageFormat::Bc2;case D3DFMT_DXT5:return ImageFormat::Bc3;
    case D3DFMT_L8:return ImageFormat::R8;case D3DFMT_A8L8:return ImageFormat::Rg8;case D3DFMT_A8:return ImageFormat::Alpha8;
    case D3DFMT_R32F:return ImageFormat::R32Float;case D3DFMT_D24S8:case D3DFMT_D24X8:return ImageFormat::DepthStencil;
    case D3DFMT_A16B16G16R16:return ImageFormat::Rgba16Unorm;case D3DFMT_A16B16G16R16F:return ImageFormat::Rgba16Float;
    default:throw std::runtime_error("Unsupported stock image format "+std::to_string(unsigned(f)));}
}
unsigned mipLevels(unsigned width,unsigned height,unsigned depth,unsigned requested) {
    if(!width||!height||!depth||width>16384||height>16384||depth>2048)throw std::runtime_error("Invalid stock image dimensions");
    unsigned maximum=1;for(auto n=(std::max)({width,height,depth});n>1;n>>=1)++maximum;
    if(requested>maximum)throw std::runtime_error("Invalid stock mip count");return requested?requested:maximum;
}
ImageStorage::ImageStorage(std::shared_ptr<Host> h,ImageDesc d,D3DFORMAT f,D3DPOOL p,DWORD u,bool cpuOnly)
    :Storage(h,cpuOnly?ResourceId{}:h->makeImage(d)),desc(d),format(f),pool(p),usage(u),levels(d.levels*(d.kind==ImageKind::Cube?6:1)) {
    const bool compressed=d.format==ImageFormat::Bc1||d.format==ImageFormat::Bc2||d.format==ImageFormat::Bc3;
    const unsigned pixelBytes=d.format==ImageFormat::R8||d.format==ImageFormat::Alpha8?1:d.format==ImageFormat::Rg8?2:d.format==ImageFormat::Rgba16Unorm||d.format==ImageFormat::Rgba16Float?8:4;
    for(unsigned layer=0;layer<(d.kind==ImageKind::Cube?6u:1u);++layer) for(unsigned mip=0;mip<d.levels;++mip) {
        auto& level=levels[layer*d.levels+mip];const auto w=(std::max)(1u,d.width>>mip),hgt=(std::max)(1u,d.height>>mip);
        level.rowBytes=compressed?((w+3)/4)*(d.format==ImageFormat::Bc1?8:16):w*pixelBytes;
        level.rows=compressed?(hgt+3)/4:hgt;level.depth=(std::max)(1u,d.depth>>mip);
    }
}
ImageStorage::Level& ImageStorage::level(unsigned mip,unsigned layer) {if(mip>=desc.levels||layer>=(desc.kind==ImageKind::Cube?6u:1u))throw std::runtime_error("Invalid stock image subresource");return levels[layer*desc.levels+mip];}
HRESULT ImageStorage::lock(unsigned mip,unsigned layer,D3DLOCKED_RECT* out,const RECT* box,DWORD flags) {
    if(!out)return E_POINTER;return checked([&]{std::lock_guard guard(host->mutex);auto& l=level(mip,layer);if(l.locked)throw std::runtime_error("Stock image already locked");
        if((flags&D3DLOCK_READONLY)&&desc.renderTarget) {if(mip||layer)throw std::runtime_error("Render-target readback requires base surface");host->readback(id,l.bytes);}
        const auto size=std::size_t(l.rowBytes)*l.rows*l.depth;if(l.bytes.empty())l.bytes.resize(size);
        unsigned offset=0;
        if(box) {const auto w=(std::max)(1u,desc.width>>mip),h=(std::max)(1u,desc.height>>mip);
            if(box->left<0||box->top<0||box->right<=box->left||box->bottom<=box->top||unsigned(box->right)>w||unsigned(box->bottom)>h)throw std::runtime_error("Invalid stock texture lock rectangle");
            if(desc.format==ImageFormat::Bc1||desc.format==ImageFormat::Bc2||desc.format==ImageFormat::Bc3) {if((box->left%4)||(box->top%4))throw std::runtime_error("Compressed lock rectangle is not block aligned");offset=(box->top/4)*l.rowBytes+(box->left/4)*(desc.format==ImageFormat::Bc1?8:16);}
            else offset=box->top*l.rowBytes+box->left*(l.rowBytes/w);
        }
        l.locked=true;l.readOnly=(flags&D3DLOCK_READONLY)!=0;l.noDirty=(flags&D3DLOCK_NO_DIRTY_UPDATE)!=0;out->Pitch=l.rowBytes;out->pBits=l.bytes.data()+offset;
    });
}
HRESULT ImageStorage::lockBox(unsigned mip,D3DLOCKED_BOX* out,const D3DBOX* box,DWORD flags) {
    if(!out)return E_POINTER;return checked([&]{std::lock_guard guard(host->mutex);auto& l=level(mip,0);if(l.locked)throw std::runtime_error("Stock volume already locked");if(l.bytes.empty())l.bytes.resize(std::size_t(l.rowBytes)*l.rows*l.depth);
        unsigned offset=0;if(box){const auto w=(std::max)(1u,desc.width>>mip),h=(std::max)(1u,desc.height>>mip);if(box->Right<=box->Left||box->Bottom<=box->Top||box->Back<=box->Front||box->Right>w||box->Bottom>h||box->Back>l.depth)throw std::runtime_error("Invalid stock volume lock box");
            if(desc.format==ImageFormat::Bc1||desc.format==ImageFormat::Bc2||desc.format==ImageFormat::Bc3)throw std::runtime_error("Partial compressed volume lock is not implemented");offset=box->Front*l.rowBytes*l.rows+box->Top*l.rowBytes+box->Left*(l.rowBytes/w);}
        l.locked=true;l.readOnly=(flags&D3DLOCK_READONLY)!=0;l.noDirty=(flags&D3DLOCK_NO_DIRTY_UPDATE)!=0;out->RowPitch=l.rowBytes;out->SlicePitch=l.rowBytes*l.rows;out->pBits=l.bytes.data()+offset;
    });
}
HRESULT ImageStorage::unlock(unsigned mip,unsigned layer) {
    return checked([&]{std::lock_guard guard(host->mutex);auto& l=level(mip,layer);if(!l.locked)throw std::runtime_error("Stock image is not locked");l.locked=false;
        if(id&&!l.readOnly)host->uploadImage(shared_from_this(),mip,layer,l.bytes);
        if(!l.readOnly&&!l.noDirty)l.dirty=true;
    });
}
HRESULT ImageStorage::markDirty(const D3DBOX* box,unsigned layer) {
    return checked([&]{std::lock_guard guard(host->mutex);level(0,layer);
        if(box&&(box->Right<=box->Left||box->Bottom<=box->Top||box->Back<=box->Front||box->Right>desc.width||box->Bottom>desc.height||box->Back>desc.depth))throw std::runtime_error("Invalid stock dirty texture region");
        // CPU shadows preserve unchanged texels. Whole-subresource transfers
        // conservatively cover the dirty region and its derived mip regions.
        for(unsigned mip=0;mip<desc.levels;++mip)level(mip,layer).dirty=true;
    });
}
D3DSURFACE_DESC ImageStorage::surfaceDesc(unsigned mip) const {return {format,D3DRTYPE_SURFACE,usage,pool,D3DMULTISAMPLE_TYPE(desc.samples==1?0:desc.samples),0,(std::max)(1u,desc.width>>mip),(std::max)(1u,desc.height>>mip)};}
Declaration::Declaration(const D3DVERTEXELEMENT9* source) {
    if(!source)throw std::runtime_error("Null stock vertex declaration");
    const VertexFormat types[]={VertexFormat::Float,VertexFormat::Float2,VertexFormat::Float3,VertexFormat::Float4,VertexFormat::Color,VertexFormat::Ubyte4,VertexFormat::Short2,VertexFormat::Short4,VertexFormat::Ubyte4N,VertexFormat::Short2N,VertexFormat::Short4N,VertexFormat::Ushort2N,VertexFormat::Ushort4N,VertexFormat::Udec3,VertexFormat::Dec3N,VertexFormat::Half2,VertexFormat::Half4};
    for(unsigned n=0;n<64;++n) {const auto e=source[n];original.push_back(e);if(e.Stream==255)return;
        if(e.Type>=std::size(types)||e.Method!=D3DDECLMETHOD_DEFAULT||e.Usage>D3DDECLUSAGE_SAMPLE||e.Stream>=16)throw std::runtime_error("Unsupported stock vertex element");
        elements.push_back({e.Stream,e.Offset,e.UsageIndex,types[e.Type],Semantic(e.Usage)});
    }throw std::runtime_error("Stock vertex declaration lacks bounded end marker");
}
ResourceId resourceId(IUnknown* object) {if(!object)return {};auto* r=dynamic_cast<Resource*>(object);if(!r)throw std::runtime_error("Foreign graphics resource passed to native Vulkan");return r->id();}
}
