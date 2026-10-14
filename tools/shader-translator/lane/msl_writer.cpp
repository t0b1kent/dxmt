// Local open MSL writer and its actual Metal resource slot map.
// Uses the public API of the lane's patched MIT/Apache SPIRV-Cross.
#include "spirv_msl.hpp"
#include <fstream>
#include <iostream>
#include <vector>

int main(int argc,char **argv)
{
    if(argc!=4) return 2;
    std::ifstream input(argv[1],std::ios::binary|std::ios::ate);
    if(!input) return 3;
    auto size=input.tellg();if(size<20||size%4) return 4;
    std::vector<uint32_t> words(size/4);input.seekg(0);input.read(reinterpret_cast<char*>(words.data()),size);
    if(!input) return 5;
    try {
        spirv_cross::CompilerMSL c(std::move(words));
        auto options=c.get_msl_options();options.msl_version=spirv_cross::CompilerMSL::Options::make_msl_version(3,1);
        c.set_msl_options(options);
        auto entries=c.get_entry_points_and_stages();if(entries.size()!=1)return 6;
        c.set_entry_point(entries[0].name,entries[0].execution_model);
        auto active=c.get_active_interface_variables();auto resources=c.get_shader_resources();
        auto source=c.compile();
        std::ofstream msl(argv[2]);msl<<source;if(!msl)return 7;
        std::ofstream out(argv[3]);out<<"{\"version\":1,\"resources\":[";bool first=true;
        auto emit=[&](const spirv_cross::SmallVector<spirv_cross::Resource>& rr,const char* kind){
            for(auto&r:rr){
                if(!first)out<<",";first=false;
                out<<"{\"variable\":"<<r.id<<",\"kind\":\""<<kind<<"\",\"set\":"<<c.get_decoration(r.id,spv::DecorationDescriptorSet)
                   <<",\"binding\":"<<c.get_decoration(r.id,spv::DecorationBinding)
                   <<",\"active\":"<<(active.count(r.id)?"true":"false")
                   <<",\"metal_slot\":"<<c.get_automatic_msl_resource_binding(r.id)
                   <<",\"metal_secondary\":"<<c.get_automatic_msl_resource_binding_secondary(r.id)<<"}";
            }
        };
        emit(resources.uniform_buffers,"buffer");emit(resources.storage_buffers,"buffer");
        emit(resources.separate_images,"texture");emit(resources.storage_images,"texture");
        emit(resources.separate_samplers,"sampler");emit(resources.sampled_images,"combined");
        out<<"]}\n";if(!out)return 8;
    } catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 9;}
    return 0;
}
