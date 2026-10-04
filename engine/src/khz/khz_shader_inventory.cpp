#include "khz_shader_inventory.h"
#include "gfx/gfx_hash.h"
#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <gfx_d3d/r_material.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>

void KHZ_CaptureStockShader(const char* name,const std::uint32_t* program,unsigned words,unsigned renderer,const char* stage) {
    if(!Dvar_GetBool("r_vkShaderInventory")||!program||!words) return;
    try {
        if(words>65536) throw std::runtime_error("Stock shader exceeds bytecode bound");
        const auto digest=gfx::sha256(std::as_bytes(std::span(program,words)));
        const auto home=std::filesystem::path(Dvar_GetString("fs_homepath"))/"vulkan"/"shader-inventory";
        static std::mutex mutex;static std::set<std::string> seen;
        std::lock_guard guard(mutex); const auto identity=home.string()+digest+stage+(name?name:"");if(seen.contains(identity)) return;
        std::filesystem::create_directories(home); const auto binary=home/(digest+".dxbc");
        if(!std::filesystem::exists(binary)) {std::ofstream out(binary,std::ios::binary);out.write(reinterpret_cast<const char*>(program),words*4);if(!out) throw std::runtime_error("Cannot save stock shader bytecode");}
        const auto index=home/"inventory.csv";const bool header=!std::filesystem::exists(index);std::ofstream out(index,std::ios::app);
        if(header) out<<"sha256,stage,renderer,words,name\n";
        std::string escaped=name?name:"";for(std::size_t n=0;(n=escaped.find('"',n))!=std::string::npos;n+=2) escaped.insert(n,1,'"');
        out<<digest<<','<<stage<<','<<renderer<<','<<words<<",\""<<escaped<<"\"\n";if(!out) throw std::runtime_error("Cannot write stock shader inventory");seen.insert(identity);
    } catch(const std::exception& e) { Com_Error(ERR_FATAL,"Stock Vulkan shader inventory failed: %s",e.what()); }
}
void KHZ_CaptureStockPass(const MaterialPass* pass) {
    if(!Dvar_GetBool("r_vkShaderInventory")||!pass) return;
    try {
        const auto home=std::filesystem::path(Dvar_GetString("fs_homepath"))/"vulkan"/"shader-inventory";
        auto digest=[](const auto* shader) { if(!shader||!shader->prog.loadDef.program||!shader->prog.loadDef.programSize) return std::string{};return gfx::sha256(std::span(static_cast<const std::byte*>(shader->prog.loadDef.program),std::size_t(shader->prog.loadDef.programSize)*4)); };
        const auto vs=digest(pass->vertexShader),ps=digest(pass->pixelShader);
        for(unsigned type=0;type<16;++type) {
            if(!pass->vertexDecl||!pass->vertexDecl->routing.decl[type]) continue;
            D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH+1]{};UINT count=std::size(elements);
            if(FAILED(pass->vertexDecl->routing.decl[type]->GetDeclaration(elements,&count))||count>std::size(elements)) throw std::runtime_error("Cannot capture stock vertex declaration");
            std::string row="{\"vertex\":\""+vs+"\",\"pixel\":\""+ps+"\",\"decl_type\":"+std::to_string(type)+",\"elements\":[";
            for(unsigned e=0;e+1<count;++e) {if(e) row+=',';const auto& d=elements[e];row+="["+std::to_string(d.Stream)+","+std::to_string(d.Offset)+","+std::to_string(d.Type)+","+std::to_string(d.Usage)+","+std::to_string(d.UsageIndex)+"]";}
            row+="]}\n";
            static std::mutex mutex;static std::set<std::string> seen;std::lock_guard guard(mutex);const auto key=home.string()+row;if(seen.contains(key)) continue;
            std::filesystem::create_directories(home);std::ofstream out(home/"passes.jsonl",std::ios::app);out<<row;if(!out) throw std::runtime_error("Cannot capture stock material linkage");seen.insert(key);
        }
    } catch(const std::exception& e) { Com_Error(ERR_FATAL,"Stock Vulkan material inventory failed: %s",e.what()); }
}
