#include "vk_shader.h"
#define SPV_ENABLE_UTILITY_CODE
#include <spirv/unified1/spirv.hpp>
#include <unordered_map>
#include <stdexcept>

namespace gfx::vk {
std::vector<std::uint32_t> depthCompare(std::span<const std::uint32_t> source,unsigned samplerMask) {
    if(!samplerMask)return {source.begin(),source.end()};
    if(source.size()<5||source[0]!=0x07230203||samplerMask>65535)throw std::runtime_error("Invalid shadow sampler lowering input");
    using namespace spv;
    std::unordered_map<unsigned,unsigned> bindings,pointers,sampledTypes,resultTypes,vectorCounts;
    std::unordered_map<unsigned,unsigned> projectiveSources;
    std::unordered_map<unsigned,std::vector<unsigned>> images;
    std::uint32_t floatType=0,vector2=0,bound=source[3];auto id=[&]{return bound++;};
    for(std::size_t at=5;at<source.size();) {
        const auto count=source[at]>>16;const auto op=Op(source[at]&65535);
        if(!count||at+count>source.size())throw std::runtime_error("Malformed shadow SPIR-V");
        bool result=false,type=false;HasResultAndType(op,&result,&type);
        if(result&&type)resultTypes[source[at+2]]=source[at+1];
        if(op==OpDecorate&&count==4&&source[at+2]==DecorationBinding)bindings[source[at+1]]=source[at+3];
        if(op==OpTypeFloat&&source[at+2]==32)floatType=source[at+1];
        if(op==OpTypeVector) {vectorCounts[source[at+1]]=source[at+3];if(source[at+2]==floatType&&source[at+3]==2)vector2=source[at+1];}
        if(op==OpTypePointer)pointers[source[at+1]]=source[at+3];
        if(op==OpTypeSampledImage)sampledTypes[source[at+1]]=source[at+2];
        if(op==OpTypeImage)images[source[at+1]]={source.begin()+at,source.begin()+at+count};
        // The pinned translator lowers TEXLDP's xyzw to xyw for ordinary
        // color sampling. Recover the original z comparison coordinate.
        if(op==OpVectorShuffle&&count==8&&source[at+3]==source[at+4]&&source[at+5]==0&&source[at+6]==1&&source[at+7]==3)
            projectiveSources[source[at+2]]=source[at+3];
        at+=count;
    }
    if(!floatType)throw std::runtime_error("Shadow sampler has no float type");
    struct Types {unsigned image,sampled,pointer;};
    std::unordered_map<unsigned,Types> clones,variables,loads;
    std::vector<std::uint32_t> out(source.begin(),source.begin()+5);
    auto emit=[](std::vector<std::uint32_t>& words,Op op,std::initializer_list<unsigned> operands){words.push_back((unsigned(operands.size()+1)<<16)|op);words.insert(words.end(),operands);};
    for(std::size_t at=5;at<source.size();) {
        const auto count=source[at]>>16;const auto op=Op(source[at]&65535);
        if(op==OpVariable&&source[at+3]==StorageClassUniformConstant&&bindings.contains(source[at+2])&&
            bindings[source[at+2]]<16&&(samplerMask&(1u<<bindings[source[at+2]]))) {
            const auto pointer=source[at+1];
            if(!clones.contains(pointer)) {
                if(!pointers.contains(pointer)||!sampledTypes.contains(pointers[pointer]))throw std::runtime_error("Unsupported shadow sampler declaration");
                const auto original=sampledTypes[pointers[pointer]];
                if(!images.contains(original)||images[original].size()!=9||images[original][3]!=Dim2D||images[original][5]||images[original][6])
                    throw std::runtime_error("Stock shadow sampling requires a single-sample 2D image");
                Types clone{id(),id(),id()};clones[pointer]=clone;
                auto image=images[original];image[1]=clone.image;image[4]=1;out.insert(out.end(),image.begin(),image.end());
                emit(out,OpTypeSampledImage,{clone.sampled,clone.image});emit(out,OpTypePointer,{clone.pointer,StorageClassUniformConstant,clone.sampled});
                if(!vector2){vector2=id();emit(out,OpTypeVector,{vector2,floatType,2});}
            }
            variables[source[at+2]]=clones[pointer];
            out.push_back(source[at]);out.push_back(clones[pointer].pointer);out.insert(out.end(),source.begin()+at+2,source.begin()+at+count);
        } else if(op==OpLoad&&variables.contains(source[at+3])) {
            loads[source[at+2]]=variables[source[at+3]];
            out.push_back(source[at]);out.push_back(loads[source[at+2]].sampled);out.insert(out.end(),source.begin()+at+2,source.begin()+at+count);
        } else if(op==OpCopyObject&&loads.contains(source[at+3])) {
            loads[source[at+2]]=loads[source[at+3]];
            emit(out,OpCopyObject,{loads[source[at+2]].sampled,source[at+2],source[at+3]});
        } else if((op==OpImageSampleImplicitLod||op==OpImageSampleExplicitLod||op==OpImageSampleProjImplicitLod||op==OpImageSampleProjExplicitLod)&&loads.contains(source[at+3])) {
            const bool projected=op==OpImageSampleProjImplicitLod||op==OpImageSampleProjExplicitLod;
            auto coord=source[at+4];if(projected&&projectiveSources.contains(coord))coord=projectiveSources[coord];
            const auto coordType=resultTypes.contains(coord)?resultTypes[coord]:0;
            if(!vectorCounts.contains(coordType)||vectorCounts[coordType]<3||(projected&&vectorCounts[coordType]!=4))
                throw std::runtime_error("Stock shadow sample has no comparison/projective coordinate");
            const auto x=id(),y=id(),reference=id();emit(out,OpCompositeExtract,{floatType,x,coord,0});emit(out,OpCompositeExtract,{floatType,y,coord,1});emit(out,OpCompositeExtract,{floatType,reference,coord,2});
            auto u=x,v=y,ref=reference;
            if(projected) {
                const auto q=id();u=id();v=id();ref=id();emit(out,OpCompositeExtract,{floatType,q,coord,3});
                emit(out,OpFDiv,{floatType,u,x,q});emit(out,OpFDiv,{floatType,v,y,q});emit(out,OpFDiv,{floatType,ref,reference,q});
            }
            const auto uv=id(),value=id();emit(out,OpCompositeConstruct,{vector2,uv,u,v});
            const bool explicitLod=op==OpImageSampleExplicitLod||op==OpImageSampleProjExplicitLod;
            out.push_back(((count+1)<<16)|(explicitLod?OpImageSampleDrefExplicitLod:OpImageSampleDrefImplicitLod));
            out.insert(out.end(),{floatType,value,source[at+3],uv,ref});out.insert(out.end(),source.begin()+at+5,source.begin()+at+count);
            // D3D9 depth comparisons replicate the filtered result to RGBA.
            emit(out,OpCompositeConstruct,{source[at+1],source[at+2],value,value,value,value});
        } else out.insert(out.end(),source.begin()+at,source.begin()+at+count);
        at+=count;
    }
    if(variables.empty())throw std::runtime_error("Requested shadow sampler was not declared");
    out[3]=bound;validateSpirv(out,"stock hardware shadow comparison");return out;
}
}
