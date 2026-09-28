#include "build_metadata.hpp"
#include "build_metadata_manifest.hpp"
#include "platform_support.hpp"
#include <fstream>
#include <stdexcept>
#include <string>

namespace {
std::vector<uint8_t> read_verified(const std::filesystem::path& path,std::size_t size,std::string_view expected) {
    if(!std::filesystem::is_regular_file(path)||std::filesystem::file_size(path)!=size)
        throw std::runtime_error("Build metadata missing or wrong size: "+path.string());
    std::ifstream input(path,std::ios::binary);
    std::vector<uint8_t> data(size);
    if(!input.read(reinterpret_cast<char*>(data.data()),static_cast<std::streamsize>(size)) || input.peek()!=std::char_traits<char>::eof())
        throw std::runtime_error("Build metadata read failed: "+path.string());
    if(tooie::platform::digest(data,true)!=expected)
        throw std::runtime_error("Build metadata SHA256 mismatch: "+path.string());
    return data;
}
}
namespace tooie::build_metadata {
std::string_view identity(){return detail::identity;}
std::string_view expected_hash(File file){return detail::files.at(static_cast<unsigned>(file)).sha256;}
std::string_view relative_path(File file){return detail::files.at(static_cast<unsigned>(file)).path;}
Bundle Bundle::load_for_executable(const std::filesystem::path& executable) {
    Bundle result;
    auto sibling=std::filesystem::absolute(executable).parent_path()/"runtime-data"/detail::identity;
    // symlink_status also detects a dangling link; an invalid selected sibling
    // must fail closed, rather than being treated as absent.
    const auto status=std::filesystem::symlink_status(sibling);
    if (status.type()==std::filesystem::file_type::not_found)
        throw std::runtime_error("Required runtime metadata bundle is missing beside the executable: "+sibling.string());
    result.directory_=sibling;
    read_verified(result.directory_/"manifest.json",detail::manifest_bytes,detail::identity);
    for(unsigned i=0;i<detail::files.size();++i) {
        const auto& spec=detail::files[i];
        result.data_[i]=read_verified(result.directory_/spec.path,spec.bytes,spec.sha256);
    }
    return result;
}
std::span<const uint8_t> Bundle::bytes(File file) const{return data_.at(static_cast<unsigned>(file));}
std::string_view Bundle::text(File file) const {
    const auto content=bytes(file);
    return {reinterpret_cast<const char*>(content.data()),content.size()};
}
const Bundle& current(){static const Bundle value=Bundle::load_for_executable(platform::executable_path());return value;}
}
