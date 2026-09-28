#include "build_metadata.hpp"
#include "platform_support.hpp"
#include <fstream>
#include <iostream>
#include <iomanip>
#include <stdexcept>

int main(int argc,char** argv) {
    if(argc<2||argc>3)return 2;
    try {
        if(std::string_view(argv[1])=="--build-metadata-info") {
            const auto executable=tooie::platform::executable_path();
            const auto bundle=tooie::build_metadata::Bundle::load_for_executable(executable);
            std::cout<<"{\"schema\":1,\"identity\":"<<std::quoted(std::string(tooie::build_metadata::identity()))
                <<",\"directory\":"<<std::quoted(bundle.directory().string())
                <<",\"executable_sha256\":"<<std::quoted(tooie::platform::file_sha256(executable))<<",\"files\":[";
            for(unsigned i=0;i<static_cast<unsigned>(tooie::build_metadata::File::Count);++i) {
                auto file=static_cast<tooie::build_metadata::File>(i);
                if(i)std::cout<<',';
                std::cout<<"{\"path\":"<<std::quoted(std::string(tooie::build_metadata::relative_path(file)))
                    <<",\"bytes\":"<<bundle.bytes(file).size()<<",\"sha256\":"
                    <<std::quoted(std::string(tooie::build_metadata::expected_hash(file)))<<'}';
            }
            std::cout<<"]}\n";return 0;
        }
        auto bundle=tooie::build_metadata::Bundle::load_for_executable(argv[1]);
        if(argc==3) {
            // A later change to the file cannot replace the already verified buffer.
            std::ofstream out(bundle.directory()/"metadata/core2.json",std::ios::binary|std::ios::trunc);
            out<<"changed after verification";
        }
        std::cout<<"PASS "<<bundle.directory().string()<<'\n';
        for(unsigned i=0;i<static_cast<unsigned>(tooie::build_metadata::File::Count);++i) {
            auto file=static_cast<tooie::build_metadata::File>(i);
            const auto digest=tooie::platform::digest(bundle.bytes(file),true);
            if(digest!=tooie::build_metadata::expected_hash(file))throw std::runtime_error("Verified buffer changed");
            std::cout<<digest<<'\n';
        }
    }catch(const std::exception& error) {std::cerr<<"REJECT "<<error.what()<<'\n';return 3;}
}
