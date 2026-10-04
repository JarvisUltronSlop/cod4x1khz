#pragma once
#include "legacy_interfaces.h"
#include "gfx_backend.h"
#include <memory>
#include <mutex>
#include <cstring>
#include <algorithm>

namespace gfx::legacy {
struct Storage;
class Host {
public:
    std::recursive_mutex mutex;
    virtual ~Host() = default;
    virtual ResourceId makeBuffer(unsigned bytes,bool index,bool dynamic) = 0;
    virtual ResourceId makeImage(const ImageDesc&) = 0;
    virtual ResourceId makeShader(std::span<const std::uint32_t>) = 0;
    virtual void release(ResourceId) noexcept = 0;
    virtual void updateBuffer(const std::shared_ptr<Storage>&,unsigned,std::span<const std::byte>,bool discard,bool noOverwrite) = 0;
    virtual void uploadImage(const std::shared_ptr<Storage>&,unsigned,unsigned,std::span<const std::byte>) = 0;
    virtual void readback(ResourceId,std::vector<std::byte>&) = 0;
};
template<class F> HRESULT checked(F&& action) noexcept {
    try { action();return S_OK;} catch(const std::exception& e) {return unsupported(e.what());} catch(...) {return unsupported("Unknown native graphics failure");}
}
struct Storage : std::enable_shared_from_this<Storage> {
    std::shared_ptr<Host> host;ResourceId id;
    explicit Storage(std::shared_ptr<Host> h,ResourceId r):host(std::move(h)),id(r){}
    virtual ~Storage(){if(id) host->release(id);}
};
class Resource {
public:
    std::shared_ptr<Storage> storage;
    explicit Resource(std::shared_ptr<Storage> s):storage(std::move(s)){}
    virtual ~Resource() = default;
    ResourceId id() const{return storage->id;}
};
ImageFormat imageFormat(D3DFORMAT);
unsigned mipLevels(unsigned width,unsigned height,unsigned depth,unsigned requested);
struct ImageStorage : Storage {
    ImageDesc desc;D3DFORMAT format;D3DPOOL pool;DWORD usage;
    struct Level {std::vector<std::byte> bytes;unsigned rowBytes=0,rows=0,depth=0;bool locked=false,readOnly=false,noDirty=false,dirty=true;};
    std::vector<Level> levels;
    ImageStorage(std::shared_ptr<Host>,ImageDesc,D3DFORMAT,D3DPOOL,DWORD,bool cpuOnly=false);
    Level& level(unsigned mip,unsigned layer);
    HRESULT lock(unsigned mip,unsigned layer,D3DLOCKED_RECT*,const RECT*,DWORD);
    HRESULT lockBox(unsigned mip,D3DLOCKED_BOX*,const D3DBOX*,DWORD);
    HRESULT unlock(unsigned mip,unsigned layer);
    HRESULT markDirty(const D3DBOX*,unsigned layer=0);
    D3DSURFACE_DESC surfaceDesc(unsigned mip) const;
};
class Surface final : public IDirect3DSurface9Base,public Resource {
    std::shared_ptr<ImageStorage> image;unsigned mip,layer;IUnknown* container;
public:
    Surface(std::shared_ptr<ImageStorage> s,unsigned m=0,unsigned l=0,IUnknown* c=nullptr):Resource(s),image(std::move(s)),mip(m),layer(l),container(c){if(container)container->AddRef();}
    ~Surface(){if(container)container->Release();}
    HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC* d) override {if(!d)return E_POINTER;*d=image->surfaceDesc(mip);return S_OK;}
    HRESULT STDMETHODCALLTYPE GetContainer(REFIID id,void** p) override {if(!p)return E_POINTER;*p=nullptr;return container?container->QueryInterface(id,p):E_NOINTERFACE;}
    HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT* r,const RECT* box,DWORD flags) override {return image->lock(mip,layer,r,box,flags);}
    HRESULT STDMETHODCALLTYPE UnlockRect() override {return image->unlock(mip,layer);}
    std::shared_ptr<ImageStorage> data(){return image;}
};
class Volume final : public IDirect3DVolume9Base,public Resource {
    std::shared_ptr<ImageStorage> image;unsigned mip;IUnknown* container;
public:
    Volume(std::shared_ptr<ImageStorage> s,unsigned m,IUnknown* c):Resource(s),image(std::move(s)),mip(m),container(c){container->AddRef();}
    ~Volume(){container->Release();}
    HRESULT STDMETHODCALLTYPE GetDesc(D3DVOLUME_DESC* d) override {if(!d)return E_POINTER;const auto s=image->surfaceDesc(mip);*d={s.Format,D3DRTYPE_VOLUME,s.Usage,s.Pool,s.Width,s.Height,(std::max)(1u,image->desc.depth>>mip)};return S_OK;}
    HRESULT STDMETHODCALLTYPE GetContainer(REFIID id,void** p) override {return container->QueryInterface(id,p);}
    HRESULT STDMETHODCALLTYPE LockBox(D3DLOCKED_BOX* r,const D3DBOX* box,DWORD flags) override {return image->lockBox(mip,r,box,flags);}
    HRESULT STDMETHODCALLTYPE UnlockBox() override {return image->unlock(mip,0);}
};
template<class Interface,class Base> class Texture final : public Base,public Resource {
    std::shared_ptr<ImageStorage> image;DWORD lod=0;
public:
    explicit Texture(std::shared_ptr<ImageStorage> s):Resource(s),image(std::move(s)){}
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override {return image->desc.kind==ImageKind::Cube?D3DRTYPE_CUBETEXTURE:image->desc.kind==ImageKind::Volume?D3DRTYPE_VOLUMETEXTURE:D3DRTYPE_TEXTURE;}
    DWORD STDMETHODCALLTYPE GetLevelCount() override {return image->desc.levels;}
    DWORD STDMETHODCALLTYPE SetLOD(DWORD value) override {const auto old=lod;lod=(std::min)(value,DWORD(image->desc.levels-1));return old;}
    DWORD STDMETHODCALLTYPE GetLOD() override {return lod;}
    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT m,D3DSURFACE_DESC* d) {if(!d||m>=image->desc.levels)return D3DERR_INVALIDCALL;*d=image->surfaceDesc(m);return S_OK;}
    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT m,D3DVOLUME_DESC* d) {if(!d||m>=image->desc.levels)return D3DERR_INVALIDCALL;const auto s=image->surfaceDesc(m);*d={s.Format,D3DRTYPE_VOLUMETEXTURE,s.Usage,s.Pool,s.Width,s.Height,(std::max)(1u,image->desc.depth>>m)};return S_OK;}
    HRESULT STDMETHODCALLTYPE GetSurfaceLevel(UINT m,IDirect3DSurface9** out) {if(!out||m>=image->desc.levels)return D3DERR_INVALIDCALL;*out=new Surface(image,m,0,static_cast<Interface*>(this));return S_OK;}
    HRESULT STDMETHODCALLTYPE GetCubeMapSurface(D3DCUBEMAP_FACES face,UINT m,IDirect3DSurface9** out) {if(!out||m>=image->desc.levels||unsigned(face)>=6)return D3DERR_INVALIDCALL;*out=new Surface(image,m,unsigned(face),static_cast<Interface*>(this));return S_OK;}
    HRESULT STDMETHODCALLTYPE GetVolumeLevel(UINT m,IDirect3DVolume9** out) {if(!out||m>=image->desc.levels)return D3DERR_INVALIDCALL;*out=new Volume(image,m,static_cast<Interface*>(this));return S_OK;}
    HRESULT STDMETHODCALLTYPE LockRect(UINT m,D3DLOCKED_RECT* r,const RECT* box,DWORD flags) {return image->lock(m,0,r,box,flags);}
    HRESULT STDMETHODCALLTYPE LockRect(D3DCUBEMAP_FACES face,UINT m,D3DLOCKED_RECT* r,const RECT* box,DWORD flags) {return image->lock(m,unsigned(face),r,box,flags);}
    HRESULT STDMETHODCALLTYPE UnlockRect(UINT m) {return image->unlock(m,0);}
    HRESULT STDMETHODCALLTYPE UnlockRect(D3DCUBEMAP_FACES face,UINT m) {return image->unlock(m,unsigned(face));}
    HRESULT STDMETHODCALLTYPE LockBox(UINT m,D3DLOCKED_BOX* r,const D3DBOX* box,DWORD flags) {return image->lockBox(m,r,box,flags);}
    HRESULT STDMETHODCALLTYPE UnlockBox(UINT m) {return image->unlock(m,0);}
    HRESULT STDMETHODCALLTYPE AddDirtyBox(const D3DBOX* box) {return image->markDirty(box);}
    HRESULT STDMETHODCALLTYPE AddDirtyRect(const RECT* rect) {if(!rect)return image->markDirty(nullptr);const D3DBOX box{UINT(rect->left),UINT(rect->top),UINT(rect->right),UINT(rect->bottom),0,1};return image->markDirty(&box);}
    HRESULT STDMETHODCALLTYPE AddDirtyRect(D3DCUBEMAP_FACES face,const RECT* rect) {if(!rect)return image->markDirty(nullptr,unsigned(face));const D3DBOX box{UINT(rect->left),UINT(rect->top),UINT(rect->right),UINT(rect->bottom),0,1};return image->markDirty(&box,unsigned(face));}
    std::shared_ptr<ImageStorage> data(){return image;}
};
using Texture2D=Texture<IDirect3DTexture9,IDirect3DTexture9Base>;
using CubeTexture=Texture<IDirect3DCubeTexture9,IDirect3DCubeTexture9Base>;
using VolumeTexture=Texture<IDirect3DVolumeTexture9,IDirect3DVolumeTexture9Base>;
template<class Base> class Buffer final : public Base,public Resource {
    std::vector<std::byte> bytes;DWORD usage,fvf;D3DPOOL pool;D3DFORMAT format;unsigned offset=0,length=0;bool locked=false,discard=false,noOverwrite=false;
public:
    Buffer(std::shared_ptr<Host> h,unsigned count,DWORD u,D3DPOOL p,bool index,DWORD vertexFvf=0,D3DFORMAT f=D3DFMT_INDEX16)
        :Resource(std::make_shared<Storage>(h,h->makeBuffer(count,index,(u&D3DUSAGE_DYNAMIC)!=0))),bytes(count),usage(u),fvf(vertexFvf),pool(p),format(f){}
    HRESULT STDMETHODCALLTYPE Lock(UINT at,UINT count,void** out,DWORD flags) override {
        if(!out||locked||at>=bytes.size()||count>bytes.size()-at)return D3DERR_INVALIDCALL;
        offset=at;length=count?count:unsigned(bytes.size()-at);discard=(flags&D3DLOCK_DISCARD)!=0;noOverwrite=(flags&D3DLOCK_NOOVERWRITE)!=0;if(discard&&noOverwrite)return D3DERR_INVALIDCALL;locked=true;*out=bytes.data()+at;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Unlock() override {if(!locked)return D3DERR_INVALIDCALL;locked=false;return checked([&]{storage->host->updateBuffer(storage,offset,std::span(bytes).subspan(offset,length),discard,noOverwrite);});}
    HRESULT STDMETHODCALLTYPE GetDesc(D3DVERTEXBUFFER_DESC* out) {if(!out)return E_POINTER;*out={D3DFMT_VERTEXDATA,D3DRTYPE_VERTEXBUFFER,usage,pool,UINT(bytes.size()),fvf};return S_OK;}
    HRESULT STDMETHODCALLTYPE GetDesc(D3DINDEXBUFFER_DESC* out) {if(!out)return E_POINTER;*out={format,D3DRTYPE_INDEXBUFFER,usage,pool,UINT(bytes.size())};return S_OK;}
};
using VertexBuffer=Buffer<IDirect3DVertexBuffer9Base>;
using IndexBuffer=Buffer<IDirect3DIndexBuffer9Base>;
template<class Base> class Shader final : public Base,public Resource {
    std::vector<std::uint32_t> words;
public:
    Shader(std::shared_ptr<Host> h,std::span<const std::uint32_t> source):Resource(std::make_shared<Storage>(h,h->makeShader(source))),words(source.begin(),source.end()){}
    HRESULT STDMETHODCALLTYPE GetFunction(void* data,UINT* count) override {if(!count)return E_POINTER;const auto size=UINT(words.size()*4);if(!data){*count=size;return S_OK;}if(*count<size)return D3DERR_MOREDATA;std::memcpy(data,words.data(),size);*count=size;return S_OK;}
};
using VertexShader=Shader<IDirect3DVertexShader9Base>;
using PixelShader=Shader<IDirect3DPixelShader9Base>;
class Declaration final : public IDirect3DVertexDeclaration9Base {
public:
    std::vector<D3DVERTEXELEMENT9> original;std::vector<VertexElement> elements;
    explicit Declaration(const D3DVERTEXELEMENT9*);
    HRESULT STDMETHODCALLTYPE GetDeclaration(D3DVERTEXELEMENT9* data,UINT* count) override {if(!count)return E_POINTER;const auto n=UINT(original.size());if(!data){*count=n;return S_OK;}if(*count<n)return D3DERR_MOREDATA;std::memcpy(data,original.data(),n*sizeof(*data));*count=n;return S_OK;}
};
ResourceId resourceId(IUnknown* object);
}
