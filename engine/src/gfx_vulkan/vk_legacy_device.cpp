#include "gfx/legacy_factory.h"
#include "gfx/legacy_resources.h"
#include "gfx/gamma_pass.h"
#include "vk_backend.h"
#include "gfx_d3d/d3d9_backend.h"
#include <d3dx9shader.h>
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <iomanip>

namespace gfx::legacy {
namespace {
constexpr std::size_t packetLimit=8192,byteLimit=64*1024*1024;
enum Metric {Frames,Draws,GpuSamples,GpuUs,WarmUs,RecordUs,SubmitUs,AcquireUs,PresentUs,RecycleUs,OwnerChanges,OwnerWaitUs,Submissions,PrepareUs,CommandsUs,ParallelBatches,ParallelDraws,MetricCount};
std::array<std::atomic<std::uint64_t>,MetricCount> metrics{};
std::atomic<unsigned> publishedValidationErrors{0},activeStates{0};
std::atomic<std::uint64_t> traceClockUs{0};
using Timer=std::chrono::steady_clock;
std::uint64_t microseconds(Timer::time_point start){return std::uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Timer::now()-start).count());}
void record(Metric metric,std::uint64_t value){metrics[metric].fetch_add(value,std::memory_order_relaxed);}
void recordMs(Metric metric,double value){record(metric,std::uint64_t(value*1000));}
enum class Operation {Draw,Clear,Buffer,Image,Resolve,Blit,QueryBegin,QueryEnd};
struct Command {
    std::uint64_t sequence=0;
    Operation operation;DrawPacket draw;ResourceId first,second;unsigned offset=0,level=0,layer=0;bool discard=false,noOverwrite=false;
    std::span<const std::byte> bytes;std::array<float,4> rgba{};float depth=1;
    unsigned stencil=0;bool clearDepth=true,clearStencil=true;
    std::span<const ClearRect> rectangles;
    std::array<ClearRect,2> blitRects{};bool hasSourceRect=false,hasDestinationRect=false,linear=false;
};
class Reference {
    struct Entry {ResourceId source,target;ImageDesc image;std::string name;std::vector<std::uint32_t> shader;std::uint64_t draws=0;};
    std::array<Entry,16384> entries{};
    struct Pass {ResourceId vertex,pixel,color,depth;MaterialState state;std::uint64_t draws=0;};
    std::vector<Pass> passes;
public:
    d3d9::Backend backend;
    explicit Reference(const Config& config):backend(config){passes.reserve(2048);}
    ResourceId map(ResourceId source) const {if(!source)return {};const auto index=source.value&65535u;if(!index||index>entries.size()||entries[index-1].source!=source)throw std::runtime_error("Stale DX9 comparison resource");return entries[index-1].target;}
    void add(ResourceId source,ResourceId target,const ImageDesc& image={}){const auto index=source.value&65535u;if(!index||index>entries.size()||entries[index-1].source)throw std::runtime_error("DX9 comparison resource bound/generation collision");entries[index-1]={source,target,image};}
    void release(ResourceId source){const auto target=map(source);backend.release(target);entries[(source.value&65535u)-1]={};}
    const ImageDesc& image(ResourceId source) const {map(source);return entries[(source.value&65535u)-1].image;}
    const std::string& name(ResourceId source)const{map(source);return entries[(source.value&65535u)-1].name;}
    void count(ResourceId source){map(source);++entries[(source.value&65535u)-1].draws;}
    void name(ResourceId source,const char* text){map(source);entries[(source.value&65535u)-1].name=text;}
    void shader(ResourceId source,std::span<const std::uint32_t> words){map(source);entries[(source.value&65535u)-1].shader.assign(words.begin(),words.end());}
    void report(const std::filesystem::path& folder){std::ofstream labels(folder/"reference-shaders.tsv");labels<<"id\tstage\tdraws\tname\n";for(const auto& entry:entries)if(!entry.shader.empty()&&entry.draws){const auto file="reference-shader-"+std::to_string(entry.source.value)+".bin";std::ofstream words(folder/file,std::ios::binary);words.write(reinterpret_cast<const char*>(entry.shader.data()),entry.shader.size()*4);labels<<entry.source.value<<'\t'<<((entry.shader[0]>>16)==0xfffe?"vertex":"pixel")<<'\t'<<entry.draws<<'\t'<<entry.name<<'\n';}
        std::ofstream states(folder/"reference-passes.tsv");states<<"vs\tps\tcolor\tdepth\tztest\tzwrite\tzcompare\tblend\tsrc\tdst\tmask\tcull\tbias\tslope\tatest\taref\tacompare\tdraws\n";
        for(const auto& pass:passes)states<<pass.vertex.value<<'\t'<<pass.pixel.value<<'\t'<<pass.color.value<<'\t'<<pass.depth.value<<'\t'<<pass.state.depthTest<<'\t'<<pass.state.depthWrite<<'\t'<<unsigned(pass.state.depthCompare)<<'\t'<<pass.state.blend<<'\t'<<unsigned(pass.state.src)<<'\t'<<unsigned(pass.state.dst)<<'\t'<<pass.state.colorMask<<'\t'<<pass.state.cull<<'\t'<<pass.state.depthBias<<'\t'<<pass.state.slopeDepthBias<<'\t'<<pass.state.alphaTest<<'\t'<<pass.state.alphaRef<<'\t'<<unsigned(pass.state.alphaCompare)<<'\t'<<pass.draws<<'\n';
    }
    void pass(const DrawPacket& d){for(auto& pass:passes)if(pass.vertex==d.vertexShader&&pass.pixel==d.pixelShader&&pass.color==d.colorTarget&&pass.depth==d.depthTarget&&pass.state==d.state){++pass.draws;return;}if(passes.size()==2048)throw std::runtime_error("Bounded diagnostic material pass inventory exhausted");passes.push_back({d.vertexShader,d.pixelShader,d.colorTarget,d.depthTarget,d.state,1});}
    std::vector<ResourceId> floatImages() const {std::vector<ResourceId> result;for(const auto& entry:entries)if(entry.source&&entry.image.format==ImageFormat::R32Float&&entry.image.renderTarget&&entry.image.samples==1)result.push_back(entry.source);return result;}
    void execute(const Command& c){
        switch(c.operation){
        case Operation::Draw:{auto d=c.draw;pass(d);count(d.vertexShader);count(d.pixelShader);d.vertexShader=map(d.vertexShader);d.pixelShader=map(d.pixelShader);d.indexBuffer=map(d.indexBuffer);d.colorTarget=map(d.colorTarget);d.depthTarget=map(d.depthTarget);for(auto& id:d.vertexBuffers)id=map(id);for(auto& id:d.textures)id=map(id);backend.draw(d);break;}
        case Operation::Clear:backend.clear(map(c.first),map(c.second),c.rgba,c.depth,c.stencil,c.clearDepth,c.clearStencil,c.rectangles);break;
        case Operation::Buffer:backend.updateBuffer(map(c.first),c.offset,c.bytes,c.discard,c.noOverwrite);break;
        case Operation::Image:backend.uploadImage(map(c.first),c.level,c.layer,c.bytes);break;
        case Operation::Resolve:backend.resolve(map(c.first),map(c.second));break;
        case Operation::Blit:backend.blit(map(c.first),map(c.second),c.linear,c.hasSourceRect?&c.blitRects[0]:nullptr,c.hasDestinationRect?&c.blitRects[1]:nullptr);break;
        case Operation::QueryBegin:backend.beginQuery(map(c.first));break;
        case Operation::QueryEnd:backend.endQuery(map(c.first));break;
        }
    }
};
class State final : public Host {
public:
    struct Trace {
        double seconds=0;std::uint64_t serial=0,gpuSerial=0,display=0;
        float frameMs=0,warmMs=0,prepareMs=0,commandsMs=0,recordMs=0,submitMs=0,acquireMs=0,presentMs=0,recycleMs=0,gpuMs=0;
        unsigned draws=0,parallelBatches=0,parallelDraws=0,presentation=0;
    };
    std::vector<Trace> trace;
    std::uint64_t traceCount=0,displayCount=0;
    Timer::time_point previousPresent=Timer::now();
    vk::Backend backend;
    std::unique_ptr<Reference> reference;
    GammaPass gamma;
    std::vector<Command> commands;
    std::vector<std::byte> arena;
    std::array<std::shared_ptr<Storage>,16384> retained{};
    std::vector<unsigned> retainedSlots;
    std::size_t used=0;bool scene=false;
    std::uint64_t queuedSequence=0,submittedSequence=0;
    std::uint64_t packetEpoch=1;
    std::string failure;
    std::thread::id submissionOwner=std::this_thread::get_id();
    Config config;unsigned diagnosticPresentations=0;
    explicit State(const Config& c):backend(c),gamma(backend),arena(byteLimit),config(c){commands.reserve(packetLimit);retainedSlots.reserve(retained.size());if(c.recordFrameTimings)trace.resize(524288);if(c.compareD3D9){auto compare=c;compare.device=-1;compare.compareD3D9=false;reference=std::make_unique<Reference>(compare);}activeStates.fetch_add(1,std::memory_order_relaxed);}
    ~State(){activeStates.fetch_sub(1,std::memory_order_relaxed);}
    void timing(const FrameTiming& timing,bool present,std::uint64_t warmUs=0){
        record(Submissions,1);if(present)record(Frames,1);record(Draws,timing.draws);record(WarmUs,warmUs);recordMs(RecordUs,timing.recordMs);recordMs(SubmitUs,timing.submitMs);recordMs(AcquireUs,timing.acquireMs);recordMs(PresentUs,timing.presentMs);recordMs(RecycleUs,timing.recycleWaitMs);if(timing.gpuSerial){record(GpuSamples,1);recordMs(GpuUs,timing.gpuMs);}
        recordMs(PrepareUs,timing.prepareMs);recordMs(CommandsUs,timing.commandsMs);record(ParallelBatches,timing.parallelBatches);record(ParallelDraws,timing.parallelDraws);
        if(!trace.empty()){
            const auto now=Timer::now();const auto seconds=std::chrono::duration<double>(now.time_since_epoch()).count();
            auto& t=trace[traceCount++%trace.size()];t={seconds,timing.serial,timing.gpuSerial,displayCount,
                present?float(std::chrono::duration<double,std::milli>(now-previousPresent).count()):0.f,
                float(warmUs*.001),float(timing.prepareMs),float(timing.commandsMs),float(timing.recordMs),float(timing.submitMs),float(timing.acquireMs),float(timing.presentMs),float(timing.recycleWaitMs),float(timing.gpuMs),timing.draws,timing.parallelBatches,timing.parallelDraws,unsigned(present)};
            traceClockUs.store(std::uint64_t(seconds*1000000.),std::memory_order_relaxed);
            if(present){previousPresent=now;++displayCount;}
        }
    }
    void owner(){const auto current=std::this_thread::get_id();if(current!=submissionOwner){const auto start=Timer::now();backend.adoptOwnerAtBarrier();if(reference)reference->backend.adoptOwnerAtBarrier();submissionOwner=current;record(OwnerChanges,1);record(OwnerWaitUs,microseconds(start));}}
    ResourceId makeBuffer(unsigned bytes,bool index,bool dynamic) override {std::lock_guard guard(mutex);owner();if(!bytes||bytes>64*1024*1024)throw std::runtime_error("Invalid stock geometry size");std::vector<std::byte> zeros(bytes);const auto id=backend.createBuffer(zeros,index,dynamic);if(reference)reference->add(id,reference->backend.createBuffer(zeros,index,dynamic));if(dynamic){backend.updateBuffer(id,0,{},true);if(reference)reference->backend.updateBuffer(reference->map(id),0,{},true);}return id;}
    ResourceId makeImage(const ImageDesc& desc) override {std::lock_guard guard(mutex);owner();const auto id=backend.createImage(desc);if(reference)reference->add(id,reference->backend.createImage(desc),desc);return id;}
    ResourceId makeShader(std::span<const std::uint32_t> words) override {std::lock_guard guard(mutex);owner();const auto id=backend.createShader(words,"stock material");if(reference){reference->add(id,reference->backend.createShader(words,"stock material"));reference->shader(id,words);}return id;}
    ResourceId makeQuery(QueryType type){const auto id=backend.createQuery(type);if(reference)reference->add(id,reference->backend.createQuery(type));return id;}
    void release(ResourceId id) noexcept override {checked([&]{std::lock_guard guard(mutex);owner();if(reference)reference->release(id);backend.release(id);});}
    template<class T> std::span<const T> copy(std::span<const T> source) {
        const auto at=(used+alignof(T)-1)&~(alignof(T)-1),bytes=source.size_bytes();
        if(bytes>arena.size()||at>arena.size()-bytes)throw std::runtime_error("Bounded immutable graphics packet arena exhausted");
        std::memcpy(arena.data()+at,source.data(),bytes);used=at+bytes;return {reinterpret_cast<const T*>(arena.data()+at),source.size()};
    }
    Command& append(Operation op) {if(!failure.empty())throw std::runtime_error(failure);if(commands.size()==packetLimit)throw std::runtime_error("Bounded immutable graphics packet queue exhausted");commands.emplace_back();commands.back().operation=op;commands.back().sequence=++queuedSequence;return commands.back();}
    void keep(const std::shared_ptr<Storage>& storage) {
        if(!storage)return;const auto index=storage->id.value&65535u;
        if(!index||index>retained.size()||storage->host.get()!=this)throw std::runtime_error("Foreign resource in immutable graphics packet");
        auto& slot=retained[index-1];
        if(slot){if(slot!=storage)throw std::runtime_error("Graphics generation reused before packet retirement");return;}
        slot=storage;retainedSlots.push_back(index-1);
    }
    void keep(Command&,IUnknown* object) {if(!object)return;auto* r=dynamic_cast<Resource*>(object);if(!r)throw std::runtime_error("Foreign resource in immutable graphics packet");keep(r->storage);}
    void retirePackets(){commands.clear();used=0;++packetEpoch;for(const auto index:retainedSlots)retained[index].reset();retainedSlots.clear();}
    void updateBuffer(const std::shared_ptr<Storage>& resource,unsigned at,std::span<const std::byte> bytes,bool discard,bool noOverwrite) override {
        const auto id=resource->id;
        std::lock_guard guard(mutex);if(!scene&&commands.empty()&&std::this_thread::get_id()==submissionOwner){backend.updateBuffer(id,at,bytes,discard,noOverwrite);if(reference)reference->backend.updateBuffer(reference->map(id),at,bytes,discard,noOverwrite);return;}
        auto& c=append(Operation::Buffer);c.first=id;c.offset=at;c.discard=discard;c.noOverwrite=noOverwrite;c.bytes=copy(bytes);keep(resource);
    }
    void uploadImage(const std::shared_ptr<Storage>& resource,unsigned level,unsigned layer,std::span<const std::byte> bytes) override {
        const auto id=resource->id;
        std::lock_guard guard(mutex);
        // Loading on the submission owner stays immediate, so an entire zone
        // cannot accumulate in the bounded packet arena. Frontend updates to
        // existing assets join the next submission with immutable bytes and
        // generation leases, without a GPU wait or an owner handoff. Preserve
        // queue order even when a resource creation subsequently changes owner.
        if(!scene&&commands.empty()&&std::this_thread::get_id()==submissionOwner){backend.beginFrame();backend.uploadImage(id,level,layer,bytes);timing(backend.endFrame({}),false);if(reference)reference->backend.uploadImage(reference->map(id),level,layer,bytes);return;}
        auto& c=append(Operation::Image);c.first=id;c.level=level;c.layer=layer;c.bytes=copy(bytes);keep(resource);
    }
    void flush(ResourceId present={},unsigned width=0,unsigned height=0,bool fullscreenGamma=false) {
        std::lock_guard guard(mutex);
        if(!failure.empty())throw std::runtime_error(failure);
        try {
        if(commands.empty()&&!present)return;owner();
        // Pipeline/shader cache work precedes recording. A later loading pass
        // warms complete material variants before entering gameplay.
        const auto warmStart=Timer::now();
        for(const auto& c:commands)if(c.operation==Operation::Draw)backend.warm(c.draw);
        const auto warmUs=microseconds(warmStart);
        const bool applyGamma=present&&fullscreenGamma&&!gamma.identity();
        if(applyGamma)gamma.prepare(present,width,height);
        if(reference){reference->backend.beginFrame();for(const auto& c:commands)reference->execute(c);reference->backend.endFrame({});}
        backend.beginFrame();
        for(const auto& c:commands)switch(c.operation) {
        case Operation::Draw:backend.draw(c.draw);break;
        case Operation::Clear:backend.clear(c.first,c.second,c.rgba,c.depth,c.stencil,c.clearDepth,c.clearStencil,c.rectangles);break;
        case Operation::Buffer:backend.updateBuffer(c.first,c.offset,c.bytes,c.discard,c.noOverwrite);break;
        case Operation::Image:backend.uploadImage(c.first,c.level,c.layer,c.bytes);break;
        case Operation::Resolve:backend.resolve(c.first,c.second);break;
        case Operation::Blit:backend.blit(c.first,c.second,c.linear,c.hasSourceRect?&c.blitRects[0]:nullptr,c.hasDestinationRect?&c.blitRects[1]:nullptr);break;
        case Operation::QueryBegin:backend.beginQuery(c.first);break;
        case Operation::QueryEnd:backend.endQuery(c.first);break;
        }
        const auto output=applyGamma?gamma.draw():present;
        const auto timing=backend.endFrame(output);
        publishedValidationErrors.store(backend.validationErrors(),std::memory_order_relaxed);
        this->timing(timing,bool(present),warmUs);
        if(!commands.empty())submittedSequence=commands.back().sequence;retirePackets();
        if(present&&config.validation&&!config.cacheDirectory.empty()) {
            ++diagnosticPresentations;
            if(diagnosticPresentations==1||diagnosticPresentations==12||diagnosticPresentations==1250) {
                const auto folder=std::filesystem::path(config.cacheDirectory).parent_path()/"diagnostics";std::filesystem::create_directories(folder);
                std::ofstream(folder/"device.json")<<backend.deviceReport()<<'\n';
                auto pixels=backend.readback(present);this->timing(backend.latestTiming(),false);
                // The diagnostic captures the engine's resolved BGRA surface,
                // before any platform gamma ramp. It runs only with validation.
                std::ofstream capture(folder/("menu-"+std::to_string(diagnosticPresentations)+".bgra"),std::ios::binary);
                capture.write(reinterpret_cast<const char*>(pixels.data()),pixels.size());
            }
        }
        }catch(const std::exception& error){failure=error.what();backend.cancelFrame();retirePackets();scene=false;throw;}
    }
    void readback(ResourceId id,std::vector<std::byte>& out) override {std::lock_guard guard(mutex);flush();owner();out=backend.readback(id);timing(backend.latestTiming(),false);
        if(reference&&!config.cacheDirectory.empty()){
            const auto pixels=reference->backend.readback(reference->map(id));const auto& desc=reference->image(id);
            const auto folder=std::filesystem::path(config.cacheDirectory).parent_path()/"diagnostics";std::filesystem::create_directories(folder);
            const auto write=[&](const char* name,const std::vector<std::byte>& data){std::ofstream file(folder/name,std::ios::binary);file.write(reinterpret_cast<const char*>(data.data()),data.size());if(!file)throw std::runtime_error("Private packet comparison capture failed");};
            write("reference-d3d9.bgra",pixels);write("reference-vulkan.bgra",out);
            reference->report(folder);
            for(const auto image:reference->floatImages()){
                const auto dx=reference->backend.readback(reference->map(image)),vk=backend.readback(image);timing(backend.latestTiming(),false);const auto& desc=reference->image(image);
                const auto prefix="reference-floatz-"+std::to_string(image.value);
                write((prefix+"-d3d9.bin").c_str(),dx);write((prefix+"-vulkan.bin").c_str(),vk);
                std::ofstream(folder/(prefix+".json"))<<"{\"width\":"<<desc.width<<",\"height\":"<<desc.height<<",\"same_immutable_packets\":true}\n";
            }
            std::ofstream(folder/"reference.json")<<"{\"width\":"<<desc.width<<",\"height\":"<<desc.height<<",\"bytes\":"<<out.size()<<",\"reference_bytes\":"<<pixels.size()<<",\"same_immutable_packets\":true,\"reference_device\":"<<reference->backend.deviceReport()<<",\"vulkan_device\":"<<backend.deviceReport()<<"}\n";
        }
    }
};
class Query final : public IDirect3DQuery9Base,public Resource {
    std::shared_ptr<State> state;D3DQUERYTYPE type;
    std::uint64_t lastIssue=0;
public:
    Query(std::shared_ptr<State> s,D3DQUERYTYPE t,QueryType q):Resource(std::make_shared<Storage>(s,s->makeQuery(q))),state(std::move(s)),type(t){}
    D3DQUERYTYPE STDMETHODCALLTYPE GetType() override {return type;}
    DWORD STDMETHODCALLTYPE GetDataSize() override {return type==D3DQUERYTYPE_TIMESTAMP?8:4;}
    HRESULT STDMETHODCALLTYPE Issue(DWORD flags) override {return checked([&]{std::lock_guard guard(state->mutex);if(flags!=D3DISSUE_BEGIN&&flags!=D3DISSUE_END)throw std::runtime_error("Invalid stock query issue flag");if(flags==D3DISSUE_BEGIN&&type!=D3DQUERYTYPE_OCCLUSION)throw std::runtime_error("Invalid stock query begin");auto& c=state->append(flags==D3DISSUE_BEGIN?Operation::QueryBegin:Operation::QueryEnd);c.first=id();lastIssue=c.sequence;state->keep(c,this);});}
    HRESULT STDMETHODCALLTYPE GetData(void* out,DWORD bytes,DWORD flags) override {
        try {std::lock_guard guard(state->mutex);if(!state->failure.empty())throw std::runtime_error(state->failure);if(out&&bytes!=GetDataSize())return D3DERR_INVALIDCALL;
            // D3D9 event queries start signaled. Stock loading barriers poll
            // them before the first Issue; treating idle as pending deadlocks.
            std::uint64_t value=1;if(!lastIssue&&type==D3DQUERYTYPE_EVENT){if(out)std::memcpy(out,&value,bytes);return S_OK;}
            if(flags&D3DGETDATA_FLUSH)state->flush();
            // Never return an earlier issue's result while its replacement is
            // still queued on the CPU and absent from the submitted GPU work.
            if(lastIssue>state->submittedSequence)return S_FALSE;
            if(!state->backend.pollQuerySerialized(id(),value))return S_FALSE;if(out)std::memcpy(out,&value,bytes);return S_OK;}catch(const std::exception& e){return unsupported(e.what());}
    }
};
template<class T> void drop(T*& value){if(value)value->Release();value=nullptr;}
template<class T> void bind(T*& to,T* value){if(value)value->AddRef();drop(to);to=value;}
template<class T,unsigned N> struct Registers {
    std::array<T,N> values{};
    std::span<const T> captured;
    std::uint64_t revision=1,capturedRevision=0,capturedEpoch=0;
    HRESULT set(UINT first,const T* source,UINT count,unsigned width){
        if(!source||first>N/width||count>N/width-first)return D3DERR_INVALIDCALL;
        auto* destination=values.data()+first*width;const auto bytes=count*width*sizeof(T);
        if(std::memcmp(destination,source,bytes)){std::memcpy(destination,source,bytes);++revision;}
        return S_OK;
    }
    std::span<const T> snapshot(State& state){
        if(capturedEpoch!=state.packetEpoch||capturedRevision!=revision){captured=state.copy<T>(values);capturedEpoch=state.packetEpoch;capturedRevision=revision;}
        return captured;
    }
};
class Swapchain final : public IDirect3DSwapChain9Base {
    std::shared_ptr<State> state;IDirect3DSurface9* back=nullptr;D3DPRESENT_PARAMETERS parameters;
public:
    Swapchain(std::shared_ptr<State> s,D3DPRESENT_PARAMETERS p):state(std::move(s)),parameters(p){ImageDesc d;d.width=p.BackBufferWidth;d.height=p.BackBufferHeight;d.format=imageFormat(p.BackBufferFormat);d.renderTarget=true;d.sampled=false;back=new Surface(std::make_shared<ImageStorage>(state,d,p.BackBufferFormat,D3DPOOL_DEFAULT,D3DUSAGE_RENDERTARGET));}
    ~Swapchain(){drop(back);}
    HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT index,D3DBACKBUFFER_TYPE type,IDirect3DSurface9** out) override {if(!out||index||type!=D3DBACKBUFFER_TYPE_MONO)return D3DERR_INVALIDCALL;*out=back;back->AddRef();return S_OK;}
    HRESULT STDMETHODCALLTYPE GetPresentParameters(D3DPRESENT_PARAMETERS* out) override {if(!out)return E_POINTER;*out=parameters;return S_OK;}
    HRESULT STDMETHODCALLTYPE Present(const RECT* source,const RECT* dest,HWND overrideWindow,const RGNDATA* region,DWORD flags) override {if(source||dest||overrideWindow||region||flags)return unsupported("Unsupported stock partial/multi-window Vulkan presentation");return checked([&]{state->flush(resourceId(back),parameters.BackBufferWidth,parameters.BackBufferHeight,!parameters.Windowed);});}
};
class Device final : public IDirect3DDevice9Base {
    std::shared_ptr<State> state;IDirect3D9* factory;
    Swapchain* swap=nullptr;IDirect3DSurface9* color=nullptr,*depth=nullptr;
    std::array<IDirect3DVertexBuffer9*,16> vertices{};std::array<IDirect3DBaseTexture9*,16> textures{};
    IDirect3DIndexBuffer9* indices=nullptr;IDirect3DVertexShader9* vs=nullptr;IDirect3DPixelShader9* ps=nullptr;Declaration* declaration=nullptr;
    std::array<unsigned,16> strides{},offsets{};std::array<Sampler,16> samplers{};
    Resource *boundColor=nullptr,*boundDepth=nullptr,*boundIndices=nullptr,*boundVs=nullptr,*boundPs=nullptr;
    std::array<Resource*,16> boundVertices{},boundTextures{};
    std::array<bool,16> depthTextures{};
    static ResourceId id(Resource* resource){return resource?resource->id():ResourceId{};}
    template<class T> HRESULT setBinding(T*& object,Resource*& resource,T* value){auto* next=dynamic_cast<Resource*>(value);if(value&&(!next||next->storage->host!=state||!next->id()))return D3DERR_INVALIDCALL;bind(object,value);resource=next;return S_OK;}
    std::array<DWORD,256> renderStates{};std::array<std::array<DWORD,14>,16> samplerStates{};
    Registers<float,256*4> vertexFloats;Registers<float,224*4> pixelFloats;
    Registers<int,16*4> vertexInts,pixelInts;Registers<int,16> vertexBools,pixelBools;
    std::uint64_t declarationRevision=1,capturedDeclarationRevision=0,capturedDeclarationEpoch=0;
    std::span<const VertexElement> capturedDeclaration;
    D3DVIEWPORT9 viewport{};RECT scissor{};D3DPRESENT_PARAMETERS pp;
    MaterialState material() const {
        MaterialState s;
        s.depthTest=renderStates[D3DRS_ZENABLE]!=D3DZB_FALSE;s.depthWrite=renderStates[D3DRS_ZWRITEENABLE]!=0;s.depthCompare=Compare(renderStates[D3DRS_ZFUNC]-1);
        s.cull=renderStates[D3DRS_CULLMODE]!=D3DCULL_NONE;s.clockwise=renderStates[D3DRS_CULLMODE]==D3DCULL_CCW;s.blend=renderStates[D3DRS_ALPHABLENDENABLE]!=0;
        s.src=Blend(renderStates[D3DRS_SRCBLEND]-1);s.dst=Blend(renderStates[D3DRS_DESTBLEND]-1);s.blendOp=BlendOp(renderStates[D3DRS_BLENDOP]-1);
        const bool separate=renderStates[D3DRS_SEPARATEALPHABLENDENABLE]!=0;s.srcAlpha=separate?Blend(renderStates[D3DRS_SRCBLENDALPHA]-1):s.src;s.dstAlpha=separate?Blend(renderStates[D3DRS_DESTBLENDALPHA]-1):s.dst;s.alphaBlendOp=separate?BlendOp(renderStates[D3DRS_BLENDOPALPHA]-1):s.blendOp;
        s.colorMask=renderStates[D3DRS_COLORWRITEENABLE];s.wireframe=renderStates[D3DRS_FILLMODE]==D3DFILL_WIREFRAME;
        s.alphaTest=renderStates[D3DRS_ALPHATESTENABLE]!=0;s.alphaCompare=Compare(renderStates[D3DRS_ALPHAFUNC]-1);s.alphaRef=renderStates[D3DRS_ALPHAREF];
        s.stencil=renderStates[D3DRS_STENCILENABLE]!=0;s.stencilRef=renderStates[D3DRS_STENCILREF];s.stencilReadMask=renderStates[D3DRS_STENCILMASK];s.stencilWriteMask=renderStates[D3DRS_STENCILWRITEMASK];
        s.front={Compare(renderStates[D3DRS_STENCILFUNC]-1),StencilOp(renderStates[D3DRS_STENCILFAIL]-1),StencilOp(renderStates[D3DRS_STENCILZFAIL]-1),StencilOp(renderStates[D3DRS_STENCILPASS]-1)};
        s.back=renderStates[D3DRS_TWOSIDEDSTENCILMODE]?MaterialState::Stencil{Compare(renderStates[D3DRS_CCW_STENCILFUNC]-1),StencilOp(renderStates[D3DRS_CCW_STENCILFAIL]-1),StencilOp(renderStates[D3DRS_CCW_STENCILZFAIL]-1),StencilOp(renderStates[D3DRS_CCW_STENCILPASS]-1)}:s.front;
        s.depthBias=std::bit_cast<float>(renderStates[D3DRS_DEPTHBIAS]);s.slopeDepthBias=std::bit_cast<float>(renderStates[D3DRS_SLOPESCALEDEPTHBIAS]);return s;
    }
public:
    Device(IDirect3D9* f,Config config,D3DPRESENT_PARAMETERS p):factory(f),pp(p) {
        config.window=p.hDeviceWindow;config.vsync=p.PresentationInterval!=D3DPRESENT_INTERVAL_IMMEDIATE;
        state=std::make_shared<State>(config);factory->AddRef();swap=new Swapchain(state,p);swap->GetBackBuffer(0,D3DBACKBUFFER_TYPE_MONO,&color);boundColor=dynamic_cast<Resource*>(color);
        viewport={0,0,p.BackBufferWidth,p.BackBufferHeight,0,1};scissor={0,0,LONG(p.BackBufferWidth),LONG(p.BackBufferHeight)};
        renderStates[D3DRS_ZENABLE]=D3DZB_TRUE;renderStates[D3DRS_ZWRITEENABLE]=TRUE;renderStates[D3DRS_ZFUNC]=D3DCMP_LESSEQUAL;renderStates[D3DRS_CULLMODE]=D3DCULL_CCW;renderStates[D3DRS_FILLMODE]=D3DFILL_SOLID;
        renderStates[D3DRS_SRCBLEND]=renderStates[D3DRS_SRCBLENDALPHA]=D3DBLEND_ONE;renderStates[D3DRS_DESTBLEND]=renderStates[D3DRS_DESTBLENDALPHA]=D3DBLEND_ZERO;renderStates[D3DRS_BLENDOP]=renderStates[D3DRS_BLENDOPALPHA]=D3DBLENDOP_ADD;renderStates[D3DRS_ALPHAFUNC]=D3DCMP_ALWAYS;renderStates[D3DRS_COLORWRITEENABLE]=15;
        for(auto r:{D3DRS_STENCILFUNC,D3DRS_CCW_STENCILFUNC})renderStates[r]=D3DCMP_ALWAYS;
        for(auto r:{D3DRS_STENCILFAIL,D3DRS_STENCILZFAIL,D3DRS_STENCILPASS,D3DRS_CCW_STENCILFAIL,D3DRS_CCW_STENCILZFAIL,D3DRS_CCW_STENCILPASS})renderStates[r]=D3DSTENCILOP_KEEP;
        renderStates[D3DRS_STENCILMASK]=renderStates[D3DRS_STENCILWRITEMASK]=255;
    }
    ~Device(){checked([&]{state->flush();});checked([&]{state->owner();state->backend.waitIdle();});for(auto& r:vertices)drop(r);for(auto& r:textures)drop(r);drop(indices);drop(vs);drop(ps);drop(declaration);drop(color);drop(depth);drop(swap);drop(factory);}
    HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override {return S_OK;}
    unsigned validationErrorCount() const {return state->backend.validationErrors();}
    void writeTimings(const char* path){
        std::lock_guard guard(state->mutex);
        if(state->trace.empty())throw std::runtime_error("Frame timing capture was not enabled at startup");
        state->owner();const auto completed=state->backend.completeFrameTimings();
        const auto parent=std::filesystem::path(path).parent_path();std::filesystem::create_directories(parent);
        const auto temporary=std::filesystem::path(path).wstring()+L".part";
        std::ofstream out(std::filesystem::path(temporary),std::ios::trunc);if(!out)throw std::runtime_error("Cannot create frame timing export");
        out<<"# bounded_capacity="<<state->trace.size()<<" total_records="<<state->traceCount<<" overwritten_records="<<(state->traceCount>state->trace.size()?state->traceCount-state->trace.size():0)<<"\n";
        out<<std::setprecision(12);
        for(const auto& gpu:completed)if(gpu.serial)out<<"# completed_gpu="<<gpu.serial<<','<<float(gpu.ms)<<'\n';
        out<<"seconds,serial,gpu_serial,display_id,frame_ms,warm_ms,prepare_ms,commands_ms,record_ms,submit_ms,acquire_ms,present_ms,recycle_ms,gpu_ms,draws,parallel_batches,parallel_draws,presentation\n"<<std::setprecision(12);
        const auto start=state->traceCount>state->trace.size()?state->traceCount-state->trace.size():0;
        for(auto n=start;n<state->traceCount;++n){const auto& t=state->trace[n%state->trace.size()];
            out<<t.seconds<<','<<t.serial<<','<<t.gpuSerial<<','<<t.display<<','<<t.frameMs<<','<<t.warmMs<<','<<t.prepareMs<<','<<t.commandsMs<<','<<t.recordMs<<','<<t.submitMs<<','<<t.acquireMs<<','<<t.presentMs<<','<<t.recycleMs<<','<<t.gpuMs<<','<<t.draws<<','<<t.parallelBatches<<','<<t.parallelDraws<<','<<t.presentation<<'\n';}
        out.close();if(!out)throw std::runtime_error("Frame timing export failed");
        if(!MoveFileExW(temporary.c_str(),std::filesystem::path(path).c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Cannot finish frame timing export; partial capture is retained");
    }
    void STDMETHODCALLTYPE SetGammaRamp(UINT chain,DWORD flags,const D3DGAMMARAMP* value) override {
        if(chain||flags||!value){unsupported("Invalid stock gamma ramp");return;}
        checked([&]{std::lock_guard guard(state->mutex);GammaPass::Ramp ramp;std::memcpy(ramp[0].data(),value->red,512);std::memcpy(ramp[1].data(),value->green,512);std::memcpy(ramp[2].data(),value->blue,512);state->gamma.setRamp(ramp);});
    }
    void STDMETHODCALLTYPE GetGammaRamp(UINT chain,D3DGAMMARAMP* value) override {
        if(chain||!value){unsupported("Invalid stock gamma ramp output");return;}
        std::lock_guard guard(state->mutex);const auto& ramp=state->gamma.getRamp();std::memcpy(value->red,ramp[0].data(),512);std::memcpy(value->green,ramp[1].data(),512);std::memcpy(value->blue,ramp[2].data(),512);
    }
    UINT STDMETHODCALLTYPE GetAvailableTextureMem() override {return UINT((std::min)(state->backend.deviceLocalMemory(),std::uint64_t(UINT32_MAX)));}
    HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D9** out) override {if(!out)return E_POINTER;*out=factory;factory->AddRef();return S_OK;}
    HRESULT STDMETHODCALLTYPE GetSwapChain(UINT index,IDirect3DSwapChain9** out) override {if(!out||index)return D3DERR_INVALIDCALL;*out=swap;swap->AddRef();return S_OK;}
    HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* p,IDirect3DSwapChain9** out) override {if(!p||!out)return E_POINTER;if(p->hDeviceWindow!=pp.hDeviceWindow)return unsupported("Native Vulkan additional window");*out=swap;swap->AddRef();return S_OK;}
    UINT STDMETHODCALLTYPE GetNumberOfSwapChains() override {return 1;}
    HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT chain,UINT index,D3DBACKBUFFER_TYPE type,IDirect3DSurface9** out) override {return chain?D3DERR_INVALIDCALL:swap->GetBackBuffer(index,type,out);}
    HRESULT STDMETHODCALLTYPE BeginScene() override {std::lock_guard guard(state->mutex);if(state->scene)return D3DERR_INVALIDCALL;state->scene=true;return S_OK;}
    HRESULT STDMETHODCALLTYPE EndScene() override {std::lock_guard guard(state->mutex);if(!state->scene)return D3DERR_INVALIDCALL;state->scene=false;return S_OK;}
    HRESULT STDMETHODCALLTYPE Present(const RECT* a,const RECT* b,HWND c,const RGNDATA* d) override {return swap->Present(a,b,c,d,0);}
    HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT bytes,DWORD usage,DWORD fvf,D3DPOOL pool,IDirect3DVertexBuffer9** out,HANDLE* shared) override {if(!out||shared)return D3DERR_INVALIDCALL;*out=nullptr;return checked([&]{*out=new VertexBuffer(state,bytes,usage,pool,false,fvf);});}
    HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT bytes,DWORD usage,D3DFORMAT format,D3DPOOL pool,IDirect3DIndexBuffer9** out,HANDLE* shared) override {if(!out||shared||format!=D3DFMT_INDEX16)return D3DERR_INVALIDCALL;*out=nullptr;return checked([&]{*out=new IndexBuffer(state,bytes,usage,pool,true,0,format);});}
    HRESULT STDMETHODCALLTYPE CreateVertexDeclaration(const D3DVERTEXELEMENT9* elements,IDirect3DVertexDeclaration9** out) override {if(!out)return E_POINTER;*out=nullptr;return checked([&]{*out=new Declaration(elements);});}
    HRESULT STDMETHODCALLTYPE CreateVertexShader(const DWORD* words,IDirect3DVertexShader9** out) override {if(!words||!out)return E_POINTER;*out=nullptr;return checked([&]{auto bytes=D3DXGetShaderSize(words);if(!bytes||bytes>65536*4)throw std::runtime_error("Invalid stock vertex shader size");*out=new VertexShader(state,{reinterpret_cast<const std::uint32_t*>(words),bytes/4});});}
    HRESULT STDMETHODCALLTYPE CreatePixelShader(const DWORD* words,IDirect3DPixelShader9** out) override {if(!words||!out)return E_POINTER;*out=nullptr;return checked([&]{auto bytes=D3DXGetShaderSize(words);if(!bytes||bytes>65536*4)throw std::runtime_error("Invalid stock pixel shader size");*out=new PixelShader(state,{reinterpret_cast<const std::uint32_t*>(words),bytes/4});});}
    HRESULT STDMETHODCALLTYPE CreateQuery(D3DQUERYTYPE type,IDirect3DQuery9** out) override {
        if(type!=D3DQUERYTYPE_EVENT&&type!=D3DQUERYTYPE_OCCLUSION&&type!=D3DQUERYTYPE_TIMESTAMP)return D3DERR_NOTAVAILABLE;
        if(!out)return S_OK;*out=nullptr;return checked([&]{std::lock_guard guard(state->mutex);state->owner();*out=new Query(state,type,type==D3DQUERYTYPE_EVENT?QueryType::Event:type==D3DQUERYTYPE_OCCLUSION?QueryType::Occlusion:QueryType::Timestamp);});
    }
    HRESULT STDMETHODCALLTYPE CreateTexture(UINT w,UINT h,UINT levels,DWORD usage,D3DFORMAT format,D3DPOOL pool,IDirect3DTexture9** out,HANDLE* shared) override {if(!out||shared)return D3DERR_INVALIDCALL;*out=nullptr;return checked([&]{ImageDesc d;d.width=w;d.height=h;d.levels=mipLevels(w,h,1,levels);d.format=imageFormat(format);d.renderTarget=(usage&(D3DUSAGE_RENDERTARGET|D3DUSAGE_DEPTHSTENCIL))!=0;*out=new Texture2D(std::make_shared<ImageStorage>(state,d,format,pool,usage,pool==D3DPOOL_SYSTEMMEM));});}
    HRESULT STDMETHODCALLTYPE CreateCubeTexture(UINT edge,UINT levels,DWORD usage,D3DFORMAT format,D3DPOOL pool,IDirect3DCubeTexture9** out,HANDLE* shared) override {if(!out||shared)return D3DERR_INVALIDCALL;*out=nullptr;return checked([&]{ImageDesc d;d.width=d.height=edge;d.levels=mipLevels(edge,edge,1,levels);d.kind=ImageKind::Cube;d.format=imageFormat(format);*out=new CubeTexture(std::make_shared<ImageStorage>(state,d,format,pool,usage,pool==D3DPOOL_SYSTEMMEM));});}
    HRESULT STDMETHODCALLTYPE CreateVolumeTexture(UINT w,UINT h,UINT depth,UINT levels,DWORD usage,D3DFORMAT format,D3DPOOL pool,IDirect3DVolumeTexture9** out,HANDLE* shared) override {if(!out||shared)return D3DERR_INVALIDCALL;*out=nullptr;return checked([&]{ImageDesc d;d.width=w;d.height=h;d.depth=depth;d.levels=mipLevels(w,h,depth,levels);d.kind=ImageKind::Volume;d.format=imageFormat(format);*out=new VolumeTexture(std::make_shared<ImageStorage>(state,d,format,pool,usage,pool==D3DPOOL_SYSTEMMEM));});}
    HRESULT STDMETHODCALLTYPE UpdateTexture(IDirect3DBaseTexture9* source,IDirect3DBaseTexture9* destination) override {
        return checked([&]{std::lock_guard guard(state->mutex);
            auto* from=dynamic_cast<Resource*>(source);auto* to=dynamic_cast<Resource*>(destination);
            const auto src=from?std::dynamic_pointer_cast<ImageStorage>(from->storage):nullptr,dst=to?std::dynamic_pointer_cast<ImageStorage>(to->storage):nullptr;
            if(!src||!dst||src->host!=state||dst->host!=state||src->pool!=D3DPOOL_SYSTEMMEM||dst->pool!=D3DPOOL_DEFAULT||!dst->id||src->format!=dst->format||src->desc.kind!=dst->desc.kind||src->desc.width!=dst->desc.width||src->desc.height!=dst->desc.height||src->desc.depth!=dst->desc.depth||src->desc.levels!=dst->desc.levels)throw std::runtime_error("Unsupported stock texture transfer extent/pool/format");
            for(unsigned layer=0;layer<(src->desc.kind==ImageKind::Cube?6u:1u);++layer)for(unsigned mip=0;mip<src->desc.levels;++mip){auto& level=src->level(mip,layer);if(level.locked)throw std::runtime_error("Stock texture transfer source is locked");if(level.dirty&&!level.bytes.empty()){state->uploadImage(dst,mip,layer,level.bytes);level.dirty=false;}}
        });
    }
    HRESULT surface(UINT w,UINT h,D3DFORMAT format,D3DMULTISAMPLE_TYPE samples,DWORD usage,IDirect3DSurface9** out,bool cpuOnly=false) {if(!out)return E_POINTER;*out=nullptr;return checked([&]{ImageDesc d;d.width=w;d.height=h;d.samples=samples==D3DMULTISAMPLE_NONE?1:samples;d.format=imageFormat(format);d.renderTarget=usage!=0;d.sampled=false;*out=new Surface(std::make_shared<ImageStorage>(state,d,format,D3DPOOL_DEFAULT,usage,cpuOnly));});}
    HRESULT STDMETHODCALLTYPE CreateRenderTarget(UINT w,UINT h,D3DFORMAT f,D3DMULTISAMPLE_TYPE s,DWORD quality,BOOL lockable,IDirect3DSurface9** out,HANDLE* shared) override {return shared||quality?D3DERR_INVALIDCALL:surface(w,h,f,s,D3DUSAGE_RENDERTARGET,out);}
    HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(UINT w,UINT h,D3DFORMAT f,D3DMULTISAMPLE_TYPE s,DWORD quality,BOOL discard,IDirect3DSurface9** out,HANDLE* shared) override {return shared||quality?D3DERR_INVALIDCALL:surface(w,h,f,s,D3DUSAGE_DEPTHSTENCIL,out);}
    HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurface(UINT w,UINT h,D3DFORMAT f,D3DPOOL pool,IDirect3DSurface9** out,HANDLE* shared) override {return shared?D3DERR_INVALIDCALL:surface(w,h,f,D3DMULTISAMPLE_NONE,0,out,true);}
    HRESULT STDMETHODCALLTYPE SetRenderTarget(DWORD index,IDirect3DSurface9* value) override {if(index)return unsupported("Native Vulkan MRT");return setBinding(color,boundColor,value);}
    HRESULT STDMETHODCALLTYPE SetDepthStencilSurface(IDirect3DSurface9* value) override {return setBinding(depth,boundDepth,value);}
    HRESULT STDMETHODCALLTYPE SetViewport(const D3DVIEWPORT9* value) override {if(!value)return E_POINTER;viewport=*value;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT9* value) override {if(!value)return E_POINTER;*value=viewport;return S_OK;}
    HRESULT STDMETHODCALLTYPE SetScissorRect(const RECT* value) override {if(!value||value->right<value->left||value->bottom<value->top)return D3DERR_INVALIDCALL;scissor=*value;return S_OK;}
    HRESULT STDMETHODCALLTYPE SetStreamSource(UINT stream,IDirect3DVertexBuffer9* value,UINT at,UINT stride) override {if(stream>=16)return D3DERR_INVALIDCALL;const auto result=setBinding(vertices[stream],boundVertices[stream],value);if(FAILED(result))return result;offsets[stream]=at;strides[stream]=stride;return S_OK;}
    HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer9* value) override {return setBinding(indices,boundIndices,value);}
    HRESULT STDMETHODCALLTYPE SetVertexShader(IDirect3DVertexShader9* value) override {return setBinding(vs,boundVs,value);}
    HRESULT STDMETHODCALLTYPE SetPixelShader(IDirect3DPixelShader9* value) override {return setBinding(ps,boundPs,value);}
    HRESULT STDMETHODCALLTYPE SetVertexDeclaration(IDirect3DVertexDeclaration9* value) override {auto* d=dynamic_cast<Declaration*>(value);if(value&&!d)return D3DERR_INVALIDCALL;if(declaration!=d)++declarationRevision;bind(declaration,d);return S_OK;}
    HRESULT STDMETHODCALLTYPE SetTexture(DWORD slot,IDirect3DBaseTexture9* value) override {if(slot>=16)return D3DERR_INVALIDCALL;const auto result=setBinding(textures[slot],boundTextures[slot],value);if(FAILED(result))return result;const auto image=boundTextures[slot]?dynamic_cast<ImageStorage*>(boundTextures[slot]->storage.get()):nullptr;depthTextures[slot]=image&&image->desc.format==ImageFormat::DepthStencil;return S_OK;}
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantF(UINT first,const float* value,UINT count) override {return vertexFloats.set(first,value,count,4);}
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(UINT first,const float* value,UINT count) override {return pixelFloats.set(first,value,count,4);}
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantI(UINT first,const int* value,UINT count) override {return vertexInts.set(first,value,count,4);}
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantI(UINT first,const int* value,UINT count) override {return pixelInts.set(first,value,count,4);}
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantB(UINT first,const BOOL* value,UINT count) override {return vertexBools.set(first,value,count,1);}
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantB(UINT first,const BOOL* value,UINT count) override {return pixelBools.set(first,value,count,1);}
    HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE type,DWORD value) override;
    HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE type,DWORD* value) override {if(!value||unsigned(type)>=renderStates.size())return D3DERR_INVALIDCALL;*value=renderStates[type];return S_OK;}
    HRESULT STDMETHODCALLTYPE SetSamplerState(DWORD slot,D3DSAMPLERSTATETYPE type,DWORD value) override;
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE type,INT base,UINT minimum,UINT count,UINT first,UINT primitives) override;
    HRESULT STDMETHODCALLTYPE Clear(DWORD count,const D3DRECT* boxes,DWORD flags,D3DCOLOR rgba,float z,DWORD stencil) override {
        if(count>256||(count&&!boxes)||(flags&~(D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER|D3DCLEAR_STENCIL)))return D3DERR_INVALIDCALL;
        return checked([&]{std::lock_guard guard(state->mutex);
            std::array<ClearRect,256> clipped{};unsigned used=0;const ClearRect view{int(viewport.X),int(viewport.Y),int(viewport.X+viewport.Width),int(viewport.Y+viewport.Height)};
            if(count){for(unsigned n=0;n<count;++n){if(boxes[n].x2<boxes[n].x1||boxes[n].y2<boxes[n].y1)throw std::runtime_error("Invalid stock clear rectangle");
                ClearRect r{(std::max)(int(boxes[n].x1),view.left),(std::max)(int(boxes[n].y1),view.top),(std::min)(int(boxes[n].x2),view.right),(std::min)(int(boxes[n].y2),view.bottom)};
                if(r.right>r.left&&r.bottom>r.top)clipped[used++]=r;}}
            else if(view.right>view.left&&view.bottom>view.top)clipped[used++]=view;
            if(!used)return;
            auto& c=state->append(Operation::Clear);c.first=(flags&D3DCLEAR_TARGET)?resourceId(color):ResourceId{};c.second=(flags&(D3DCLEAR_ZBUFFER|D3DCLEAR_STENCIL))?resourceId(depth):ResourceId{};c.rgba={float((rgba>>16)&255)/255.f,float((rgba>>8)&255)/255.f,float(rgba&255)/255.f,float(rgba>>24)/255.f};c.depth=z;c.stencil=stencil;c.clearDepth=(flags&D3DCLEAR_ZBUFFER)!=0;c.clearStencil=(flags&D3DCLEAR_STENCIL)!=0;
            c.rectangles=state->copy<ClearRect>({clipped.data(),used});
            state->keep(c,color);state->keep(c,depth);
        });
    }
    HRESULT STDMETHODCALLTYPE GetRenderTargetData(IDirect3DSurface9* source,IDirect3DSurface9* destination) override {return checked([&]{auto* dest=dynamic_cast<Surface*>(destination);if(!dest)throw std::runtime_error("Invalid stock readback surface");auto& level=dest->data()->level(0,0);state->readback(resourceId(source),level.bytes);});}
    HRESULT STDMETHODCALLTYPE StretchRect(IDirect3DSurface9* source,const RECT* sourceBox,IDirect3DSurface9* destination,const RECT* destinationBox,D3DTEXTUREFILTERTYPE filter) override {
        if(!source||!destination||(filter!=D3DTEXF_NONE&&filter!=D3DTEXF_POINT&&filter!=D3DTEXF_LINEAR))return D3DERR_INVALIDCALL;
        return checked([&]{std::lock_guard guard(state->mutex);auto& c=state->append(Operation::Blit);c.first=resourceId(source);c.second=resourceId(destination);c.linear=filter==D3DTEXF_LINEAR;
            c.hasSourceRect=sourceBox!=nullptr;c.hasDestinationRect=destinationBox!=nullptr;
            if(sourceBox)c.blitRects[0]={sourceBox->left,sourceBox->top,sourceBox->right,sourceBox->bottom};
            if(destinationBox)c.blitRects[1]={destinationBox->left,destinationBox->top,destinationBox->right,destinationBox->bottom};
            state->keep(c,source);state->keep(c,destination);});
    }
};
HRESULT Device::SetRenderState(D3DRENDERSTATETYPE type,DWORD value) {
    if(unsigned(type)>=renderStates.size())return D3DERR_INVALIDCALL;
    switch(type) {
    case D3DRS_ZENABLE:case D3DRS_ZWRITEENABLE:case D3DRS_ZFUNC:case D3DRS_FILLMODE:case D3DRS_CULLMODE:case D3DRS_COLORWRITEENABLE:
    case D3DRS_ALPHABLENDENABLE:case D3DRS_SRCBLEND:case D3DRS_DESTBLEND:case D3DRS_BLENDOP:case D3DRS_SEPARATEALPHABLENDENABLE:case D3DRS_SRCBLENDALPHA:case D3DRS_DESTBLENDALPHA:case D3DRS_BLENDOPALPHA:
    case D3DRS_ALPHATESTENABLE:case D3DRS_ALPHAFUNC:case D3DRS_ALPHAREF:case D3DRS_STENCILENABLE:case D3DRS_STENCILFUNC:case D3DRS_STENCILFAIL:case D3DRS_STENCILZFAIL:case D3DRS_STENCILPASS:case D3DRS_STENCILREF:case D3DRS_STENCILMASK:case D3DRS_STENCILWRITEMASK:
    case D3DRS_TWOSIDEDSTENCILMODE:case D3DRS_CCW_STENCILFUNC:case D3DRS_CCW_STENCILFAIL:case D3DRS_CCW_STENCILZFAIL:case D3DRS_CCW_STENCILPASS:case D3DRS_DEPTHBIAS:case D3DRS_SLOPESCALEDEPTHBIAS:case D3DRS_SCISSORTESTENABLE:break;
    // Shader-based stock drawing does not use the fixed-function lighting,
    // texture or fog state. Explicitly accept only the disabled baseline.
    case D3DRS_LIGHTING:case D3DRS_FOGENABLE:case D3DRS_SRGBWRITEENABLE:case D3DRS_ADAPTIVETESS_Y:if(value)return unsupported("Native Vulkan enabled fixed-function/sRGB/alpha-to-coverage state");break;
    default:return unsupported(("Stock render state "+std::to_string(unsigned(type))).c_str());
    }
    renderStates[type]=value;return S_OK;
}
HRESULT Device::SetSamplerState(DWORD slot,D3DSAMPLERSTATETYPE type,DWORD value) {
    if(slot>=16||unsigned(type)>=14)return D3DERR_INVALIDCALL;auto& s=samplers[slot];
    switch(type) {
    case D3DSAMP_ADDRESSU:case D3DSAMP_ADDRESSV:case D3DSAMP_ADDRESSW:if(value<1||value>4)return unsupported("Stock sampler address mode");if(type==D3DSAMP_ADDRESSU)s.u=Address(value-1);else if(type==D3DSAMP_ADDRESSV)s.v=Address(value-1);else s.w=Address(value-1);break;
    case D3DSAMP_MINFILTER:case D3DSAMP_MAGFILTER:if(value!=D3DTEXF_POINT&&value!=D3DTEXF_LINEAR&&value!=D3DTEXF_ANISOTROPIC)return unsupported("Stock sampler filter");if(type==D3DSAMP_MINFILTER)s.minLinear=value!=D3DTEXF_POINT;else s.magLinear=value!=D3DTEXF_POINT;break;
    case D3DSAMP_MIPFILTER:if(value>D3DTEXF_LINEAR)return unsupported("Stock mip filter");s.mipLinear=value==D3DTEXF_LINEAR;s.maxLod=value==D3DTEXF_NONE?0:16;break;
    case D3DSAMP_MIPMAPLODBIAS:s.lodBias=std::bit_cast<float>(value);break;
    case D3DSAMP_MAXMIPLEVEL:s.minLod=float(value);break;
    case D3DSAMP_MAXANISOTROPY:s.maxAnisotropy=float(value);break;
    case D3DSAMP_BORDERCOLOR:case D3DSAMP_SRGBTEXTURE:if(value)return unsupported("Stock nonzero sampler border/sRGB state");break;
    default:return unsupported("Stock sampler state");
    }samplerStates[slot][type]=value;return S_OK;
}
HRESULT Device::DrawIndexedPrimitive(D3DPRIMITIVETYPE type,INT base,UINT minimum,UINT count,UINT first,UINT primitives) {
    if(type!=D3DPT_TRIANGLELIST||!vs||!ps||!declaration||!indices||!color)return unsupported("Invalid native stock draw state or topology");
    return checked([&]{std::lock_guard guard(state->mutex);auto& c=state->append(Operation::Draw);auto& d=c.draw;
        d.vertexShader=id(boundVs);d.pixelShader=id(boundPs);d.indexBuffer=id(boundIndices);d.colorTarget=id(boundColor);d.depthTarget=id(boundDepth);d.strides=strides;d.vertexOffsets=offsets;d.samplers=samplers;
        if(capturedDeclarationEpoch!=state->packetEpoch||capturedDeclarationRevision!=declarationRevision){capturedDeclaration=state->copy<VertexElement>(declaration->elements);capturedDeclarationEpoch=state->packetEpoch;capturedDeclarationRevision=declarationRevision;}
        d.elements=capturedDeclaration;d.vertexConstants=vertexFloats.snapshot(*state);d.pixelConstants=pixelFloats.snapshot(*state);d.vertexIntegers=vertexInts.snapshot(*state);d.pixelIntegers=pixelInts.snapshot(*state);d.vertexBooleans=vertexBools.snapshot(*state);d.pixelBooleans=pixelBools.snapshot(*state);d.immutableConstants=true;
        d.state=material();d.indexCount=primitives*3;d.firstIndex=first;d.vertexBase=base;d.vertexFirst=minimum;d.vertexCount=count;d.x=float(viewport.X);d.y=float(viewport.Y);d.width=float(viewport.Width);d.height=float(viewport.Height);d.minDepth=viewport.MinZ;d.maxDepth=viewport.MaxZ;
        d.scissor=renderStates[D3DRS_SCISSORTESTENABLE]!=0;d.scissorX=scissor.left;d.scissorY=scissor.top;d.scissorWidth=scissor.right-scissor.left;d.scissorHeight=scissor.bottom-scissor.top;
        for(unsigned s=0;s<16;++s){d.vertexBuffers[s]=id(boundVertices[s]);d.textures[s]=id(boundTextures[s]);if(boundVertices[s])state->keep(boundVertices[s]->storage);if(boundTextures[s])state->keep(boundTextures[s]->storage);
            if(depthTextures[s]){d.samplers[s].compare=true;d.samplers[s].comparison=Compare::LessEqual;}}
        for(auto* r:{boundVs,boundPs,boundIndices,boundColor,boundDepth})if(r)state->keep(r->storage);
    });
}
class Factory final : public IDirect3D9Base {
    Config config;IDirect3D9* display;
public:
    explicit Factory(const Config& c):config(c),display(Direct3DCreate9(D3D_SDK_VERSION)){if(!display)throw std::runtime_error("Stock display-mode enumerator unavailable");}
    ~Factory(){display->Release();}
    UINT STDMETHODCALLTYPE GetAdapterCount() override{return display->GetAdapterCount();}
    UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT a,D3DFORMAT f) override{return display->GetAdapterModeCount(a,f);}
    HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT a,D3DFORMAT f,UINT n,D3DDISPLAYMODE* out) override{return display->EnumAdapterModes(a,f,n,out);}
    HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT a,D3DDISPLAYMODE* out) override{return display->GetAdapterDisplayMode(a,out);}
    HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT a) override{return display->GetAdapterMonitor(a);}
    HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT a,DWORD flags,D3DADAPTER_IDENTIFIER9* out) override{return display->GetAdapterIdentifier(a,flags,out);}
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT a,D3DDEVTYPE t,D3DCAPS9* out) override{return display->GetDeviceCaps(a,t,out);}
    HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT a,D3DDEVTYPE t,D3DFORMAT displayFormat,D3DFORMAT target,BOOL windowed) override{return display->CheckDeviceType(a,t,displayFormat,target,windowed);}
    HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT a,D3DDEVTYPE t,D3DFORMAT adapter,DWORD usage,D3DRESOURCETYPE resource,D3DFORMAT f) override {try{imageFormat(f);}catch(...){return D3DERR_NOTAVAILABLE;}return display->CheckDeviceFormat(a,t,adapter,usage,resource,f);}
    HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT a,D3DDEVTYPE t,D3DFORMAT f,BOOL w,D3DMULTISAMPLE_TYPE s,DWORD* quality) override {if(s!=D3DMULTISAMPLE_NONE&&s!=D3DMULTISAMPLE_2_SAMPLES&&s!=D3DMULTISAMPLE_4_SAMPLES&&s!=D3DMULTISAMPLE_8_SAMPLES)return D3DERR_NOTAVAILABLE;if(quality)*quality=1;return S_OK;}
    HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT a,D3DDEVTYPE t,D3DFORMAT adapter,D3DFORMAT target,D3DFORMAT depth) override{try{imageFormat(target);}catch(...){return D3DERR_NOTAVAILABLE;}return depth==D3DFMT_D24S8||depth==D3DFMT_D24X8?S_OK:D3DERR_NOTAVAILABLE;}
    HRESULT STDMETHODCALLTYPE CreateDevice(UINT adapter,D3DDEVTYPE type,HWND window,DWORD behavior,D3DPRESENT_PARAMETERS* p,IDirect3DDevice9** out) override {if(!p||!out)return E_POINTER;*out=nullptr;return checked([&]{if(!p->hDeviceWindow)p->hDeviceWindow=window;if(!p->BackBufferWidth||!p->BackBufferHeight){RECT box;GetClientRect(p->hDeviceWindow,&box);p->BackBufferWidth=box.right;p->BackBufferHeight=box.bottom;}if(p->BackBufferFormat==D3DFMT_UNKNOWN)p->BackBufferFormat=D3DFMT_A8R8G8B8;*out=new Device(this,config,*p);});}
};
}
IDirect3D9* createVulkanInterface(const Config& config) {try{return new Factory(config);}catch(const std::exception& e){unsupported(e.what());return nullptr;}}
void nameResource(IUnknown* object,const char* name){if(!object||!name)return;if(auto* resource=dynamic_cast<Resource*>(object)){if(std::strlen(name)>256)return;resource->storage->debugName=name;if(const auto state=std::dynamic_pointer_cast<State>(resource->storage->host)){std::lock_guard guard(state->mutex);if(state->reference)state->reference->name(resource->id(),name);}}}
const char* frameTelemetry(){
    thread_local char text[1024];
    const char* names[]={"frames","draws","gpu_samples","gpu_us","warm_us","record_us","submit_us","acquire_us","present_us","recycle_us","owner_changes","owner_wait_us","submissions","prepare_us","commands_us","parallel_batches","parallel_draws"};
    unsigned used=std::snprintf(text,sizeof(text),"{\"backend\":\"vulkan\",\"active\":%s,\"validation_errors\":%u",activeStates.load(std::memory_order_relaxed)?"true":"false",publishedValidationErrors.load(std::memory_order_relaxed));
    for(unsigned n=0;n<MetricCount;++n)used+=std::snprintf(text+used,sizeof(text)-used,",\"%s\":%llu",names[n],metrics[n].load(std::memory_order_relaxed));
    std::snprintf(text+used,sizeof(text)-used,",\"trace_clock_us\":%llu}",traceClockUs.load(std::memory_order_relaxed));return text;
}
unsigned validationErrors(IDirect3DDevice9* device){const auto* native=dynamic_cast<Device*>(device);return native?native->validationErrorCount():UINT32_MAX;}
bool writeFrameTimings(IDirect3DDevice9* device,const char* path){auto* native=dynamic_cast<Device*>(device);if(!native)return false;return SUCCEEDED(checked([&]{native->writeTimings(path);}));}
}
