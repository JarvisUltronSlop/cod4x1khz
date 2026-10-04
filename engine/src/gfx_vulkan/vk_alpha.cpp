#include "vk_shader.h"
#include <spirv/unified1/spirv.hpp>
#include <spirv/unified1/GLSL.std.450.h>
#include <unordered_map>
#include <unordered_set>
#include <cstring>
#include <stdexcept>

namespace gfx::vk {
std::vector<std::uint32_t> fragmentState(std::span<const std::uint32_t> source,
    int comparison,bool depthBias,bool slopeBias) {
    if(source.size()<5||source[0]!=0x07230203||comparison < -1||comparison>7)
        throw std::runtime_error("Invalid fixed-function fragment lowering input");
    using namespace spv;
    std::uint32_t main=0,color=0,floatType=0,boolType=0,uintType=0,colorType=0,vector4=0,fragCoord=0,depth=0,glsl=0;
    std::unordered_map<std::uint32_t,std::uint32_t> locations,builtins,pointers,variables;
    std::unordered_set<std::uint32_t> outputs;
    for(std::size_t at=5;at<source.size();) {
        const auto count=source[at]>>16,op=source[at]&65535;
        if(!count||at+count>source.size())throw std::runtime_error("Malformed SPIR-V in fragment lowering");
        if(op==OpEntryPoint&&source[at+1]==ExecutionModelFragment)main=source[at+2];
        if(op==OpDecorate&&count==4&&source[at+2]==DecorationLocation)locations[source[at+1]]=source[at+3];
        if(op==OpDecorate&&count==4&&source[at+2]==DecorationBuiltIn)builtins[source[at+1]]=source[at+3];
        if(op==OpTypeFloat&&source[at+2]==32)floatType=source[at+1];
        if(op==OpTypeBool)boolType=source[at+1];
        if(op==OpTypeInt&&source[at+2]==32&&source[at+3]==0)uintType=source[at+1];
        if(op==OpTypePointer)pointers[source[at+1]]=source[at+3];
        if(op==OpVariable){variables[source[at+2]]=source[at+1];if(source[at+3]==StorageClassOutput)outputs.insert(source[at+2]);}
        if(op==OpExtInstImport&&count>=5&&!std::memcmp(source.data()+at+2,"GLSL.std.450",12))glsl=source[at+1];
        at+=count;
    }
    for(auto [id,pointer]:variables) {
        if(outputs.contains(id)&&locations.contains(id)&&locations[id]==0){color=id;colorType=pointers.at(pointer);}
        if(builtins.contains(id)&&builtins[id]==BuiltInFragCoord){fragCoord=id;vector4=pointers.at(pointer);}
        if(builtins.contains(id)&&builtins[id]==BuiltInFragDepth)depth=id;
    }
    // A stock pixel shader that writes oDepth replaces rasterizer depth,
    // including rasterizer bias. Preserve its explicit depth unchanged.
    if(depth)depthBias=false;
    slopeBias=depthBias&&slopeBias;
    const bool alpha=comparison>=0;
    if(!main||!floatType||(alpha&&(!color||!colorType)))
        throw std::runtime_error("Pixel shader has no required output for stock fragment state");
    if(!alpha&&!depthBias)return {source.begin(),source.end()};
    std::uint32_t bound=source[3];auto id=[&]{return bound++;};
    const auto block=id(),pointerBlock=id(),push=id(),pointerFloat=id(),zero=id(),one=id(),two=id();
    std::vector<std::uint32_t> annotations,globals,imports;
    auto emit=[](std::vector<std::uint32_t>& out,Op op,std::initializer_list<std::uint32_t> operands){out.push_back((std::uint32_t(operands.size()+1)<<16)|std::uint32_t(op));out.insert(out.end(),operands);};
    emit(annotations,OpDecorate,{block,DecorationBlock});
    for(unsigned member=0;member<3;++member)emit(annotations,OpMemberDecorate,{block,member,DecorationOffset,member*4});
    if(!boolType){boolType=id();emit(globals,OpTypeBool,{boolType});}
    if(!uintType){uintType=id();emit(globals,OpTypeInt,{uintType,32,0});}
    emit(globals,OpTypeStruct,{block,floatType,floatType,floatType});
    emit(globals,OpTypePointer,{pointerBlock,StorageClassPushConstant,block});
    emit(globals,OpTypePointer,{pointerFloat,StorageClassPushConstant,floatType});
    emit(globals,OpConstant,{uintType,zero,0});emit(globals,OpConstant,{uintType,one,1});emit(globals,OpConstant,{uintType,two,2});
    emit(globals,OpVariable,{pointerBlock,push,StorageClassPushConstant});
    std::uint32_t constantBool=0;
    if(alpha&&(comparison==0||comparison==7)){constantBool=id();emit(globals,comparison?OpConstantTrue:OpConstantFalse,{boolType,constantBool});}
    const bool addedCoord=depthBias&&!fragCoord;
    std::uint32_t zeroFloat=0,oneFloat=0;
    if(depthBias) {
        if(!vector4) {
            for(std::size_t at=5;at<source.size();at+=source[at]>>16)
                if((source[at]&65535)==OpTypeVector&&source[at+2]==floatType&&source[at+3]==4)vector4=source[at+1];
            if(!vector4){vector4=id();emit(globals,OpTypeVector,{vector4,floatType,4});}
        }
        if(addedCoord) {
            const auto pointer=id();fragCoord=id();
            emit(globals,OpTypePointer,{pointer,StorageClassInput,vector4});
            emit(globals,OpVariable,{pointer,fragCoord,StorageClassInput});
            emit(annotations,OpDecorate,{fragCoord,DecorationBuiltIn,BuiltInFragCoord});
        }
        const auto pointer=id();depth=id();
        emit(globals,OpTypePointer,{pointer,StorageClassOutput,floatType});
        emit(globals,OpVariable,{pointer,depth,StorageClassOutput});
        emit(annotations,OpDecorate,{depth,DecorationBuiltIn,BuiltInFragDepth});
        zeroFloat=id();oneFloat=id();emit(globals,OpConstant,{floatType,zeroFloat,0});emit(globals,OpConstant,{floatType,oneFloat,0x3f800000});
        if(!glsl) {
            glsl=id();imports={(6u<<16)|OpExtInstImport,glsl,0,0,0,0};
            std::memcpy(imports.data()+2,"GLSL.std.450",13);
        }
    }
    std::vector<std::uint32_t> out(source.begin(),source.begin()+5);
    bool addedAnnotations=false,addedGlobals=false,addedModes=false,inMain=false,mainLabel=false,pendingBias=false,addedBias=false;unsigned returns=0;
    for(std::size_t at=5;at<source.size();) {
        const auto count=source[at]>>16;const auto op=Op(source[at]&65535);
        if(op==OpMemoryModel)out.insert(out.end(),imports.begin(),imports.end());
        if(!addedModes&&(op==OpSource||op==OpSourceContinued||op==OpSourceExtension||op==OpName||op==OpMemberName||op==OpString||op==OpDecorate||op==OpMemberDecorate||(op>=OpTypeVoid&&op<=OpTypeForwardPointer))) {
            if(depthBias)emit(out,OpExecutionMode,{main,ExecutionModeDepthReplacing});addedModes=true;
        }
        if(!addedAnnotations&&op>=OpTypeVoid&&op<=OpTypeForwardPointer){out.insert(out.end(),annotations.begin(),annotations.end());addedAnnotations=true;}
        if(op==OpFunction){if(!addedGlobals){out.insert(out.end(),globals.begin(),globals.end());addedGlobals=true;}inMain=source[at+2]==main;}
        // OpKill can terminate neighboring lanes. Derivatives after a
        // divergent stock alpha discard are undefined; evaluate rasterizer
        // bias in the entry block while the entire quad is still active.
        // Keep all Function variables ahead of executable instructions.
        if(inMain&&op==OpLabel&&!mainLabel){mainLabel=true;pendingBias=depthBias;}
        else if(pendingBias&&op!=OpVariable&&op!=OpLine&&op!=OpNoLine) {
                pendingBias=false;addedBias=true;
                const auto coord=id(),z=id(),biasPointer=id(),bias=id();
                emit(out,OpLoad,{vector4,coord,fragCoord});emit(out,OpCompositeExtract,{floatType,z,coord,2});
                emit(out,OpAccessChain,{pointerFloat,biasPointer,push,one});emit(out,OpLoad,{floatType,bias,biasPointer});
                auto offset=bias;
                if(slopeBias) {
                    const auto dx=id(),dy=id(),ax=id(),ay=id(),slope=id(),scalePointer=id(),scale=id(),scaled=id(),sum=id();
                    emit(out,OpDPdx,{floatType,dx,z});emit(out,OpDPdy,{floatType,dy,z});
                    emit(out,OpExtInst,{floatType,ax,glsl,GLSLstd450FAbs,dx});emit(out,OpExtInst,{floatType,ay,glsl,GLSLstd450FAbs,dy});
                    emit(out,OpExtInst,{floatType,slope,glsl,GLSLstd450FMax,ax,ay});
                    emit(out,OpAccessChain,{pointerFloat,scalePointer,push,two});emit(out,OpLoad,{floatType,scale,scalePointer});
                    emit(out,OpFMul,{floatType,scaled,slope,scale});emit(out,OpFAdd,{floatType,sum,bias,scaled});offset=sum;
                }
                const auto sum=id(),clamped=id();emit(out,OpFAdd,{floatType,sum,z,offset});
                emit(out,OpExtInst,{floatType,clamped,glsl,GLSLstd450FClamp,sum,zeroFloat,oneFloat});emit(out,OpStore,{depth,clamped});
        }
        if(inMain&&op==OpReturn) {
            if(alpha) {
                const auto loaded=id(),a=id(),refPointer=id(),ref=id(),predicate=constantBool?constantBool:id(),kill=id(),merge=id();
                emit(out,OpLoad,{colorType,loaded,color});emit(out,OpCompositeExtract,{floatType,a,loaded,3});
                emit(out,OpAccessChain,{pointerFloat,refPointer,push,zero});emit(out,OpLoad,{floatType,ref,refPointer});
                const Op comparisons[]={OpNop,OpFOrdLessThan,OpFOrdEqual,OpFOrdLessThanEqual,OpFOrdGreaterThan,OpFOrdNotEqual,OpFOrdGreaterThanEqual,OpNop};
                if(!constantBool)emit(out,comparisons[comparison],{boolType,predicate,a,ref});
                emit(out,OpSelectionMerge,{merge,SelectionControlMaskNone});emit(out,OpBranchConditional,{predicate,merge,kill});emit(out,OpLabel,{kill});emit(out,OpKill,{});emit(out,OpLabel,{merge});
            }
            ++returns;
        }
        if(depthBias&&op==OpExecutionMode&&source[at+1]==main&&source[at+2]==ExecutionModeEarlyFragmentTests){at+=count;continue;}
        if(depthBias&&op==OpEntryPoint&&source[at+1]==ExecutionModelFragment&&source[at+2]==main) {
            const auto extra=addedCoord?2u:1u;out.push_back(((count+extra)<<16)|op);
            out.insert(out.end(),source.begin()+at+1,source.begin()+at+count);out.push_back(depth);if(addedCoord)out.push_back(fragCoord);
        } else out.insert(out.end(),source.begin()+at,source.begin()+at+count);
        if(op==OpFunctionEnd)inMain=false;at+=count;
    }
    if(!returns||!addedAnnotations||!addedGlobals||(depthBias&&!addedBias))throw std::runtime_error("Unsupported pixel entry point for stock fragment lowering");
    out[3]=bound;validateSpirv(out,"stock alpha/depth-bias lowering");return out;
}
std::vector<std::uint32_t> alphaTest(std::span<const std::uint32_t> source,unsigned comparison) {
    return fragmentState(source,int(comparison),false,false);
}
}
