// Disposable device fixture over actual EEPROM wrappers and save implementation.
// No game entrypoint, generated guest procedure, or user save is executed/read.
#ifndef TOOIE_SAVE_RUNTIME_SOURCE
#error Supply the selected pinned/materialized pi.cpp path
#endif
#include TOOIE_SAVE_RUNTIME_SOURCE
#include <iostream>
#include <stdexcept>

extern recomp::SaveType save_type; // Actual runtime global configured by startup.
extern "C" void osEepromProbe_recomp(uint8_t*,recomp_context*);
extern "C" void osEepromWrite_recomp(uint8_t*,recomp_context*);
extern "C" void osEepromRead_recomp(uint8_t*,recomp_context*);
extern "C" void osEepromLongWrite_recomp(uint8_t*,recomp_context*);
extern "C" void osEepromLongRead_recomp(uint8_t*,recomp_context*);

static void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(int argc,char**argv) {
    try {
        if(argc!=2)throw std::runtime_error("requires NEW_OUTPUT_DIRECTORY");
        const std::filesystem::path directory=argv[1];
        check(!std::filesystem::exists(directory),"fixture output must be new");
        std::filesystem::create_directories(directory);
        save_context.save_file_path=directory/"disposable-eeprom16k.bin";
        save_type=recomp::SaveType::Eep16k;
        check(get_save_size(recomp::get_save_type())==2048,"EEPROM16k capacity differs");
        save_context.save_buffer.resize(get_save_size(recomp::get_save_type()));
        read_save_file();
        std::vector<uint8_t> memory(0x800000);
        uint8_t* rdram=memory.data();
        recomp_context context{};
        osEepromProbe_recomp(rdram,&context);
        check(context.r2==2,"16k device probe did not return2");
        constexpr int32_t input=int32_t(0x80001000u),output=int32_t(0x80003000u);
        context.r5=0;context.r6=output;context.r7=2048;
        osEepromLongRead_recomp(rdram,&context);
        check(context.r2==0,"initial read failed");
        for(int i=0;i<2048;++i)check(uint8_t(MEM_B(i,output))==0,"runtime new-file zero convention changed");
        std::array<uint8_t,2048> expected;
        for(size_t i=0;i<expected.size();++i) {
            expected[i]=uint8_t((i*37+19)^(i>>3));
            MEM_B(i,input)=expected[i];
        }
        // First and last blocks exercise every address bit, including >4k range.
        context.r5=0;context.r6=input;
        osEepromWrite_recomp(rdram,&context);check(context.r2==0,"first block write failed");
        context.r5=255;context.r6=input+2040;
        osEepromWrite_recomp(rdram,&context);check(context.r2==0,"last block write failed");
        context.r5=1;context.r6=input+8;context.r7=2032;
        osEepromLongWrite_recomp(rdram,&context);check(context.r2==0,"middle long write failed");
        MEM_B(-1,output)=0x5A;MEM_B(2048,output)=0xA5;
        context.r5=0;context.r6=output;context.r7=2048;
        osEepromLongRead_recomp(rdram,&context);check(context.r2==0,"full read failed");
        for(size_t i=0;i<expected.size();++i)check(uint8_t(MEM_B(i,output))==expected[i],"word-swapped full read mismatch");
        check(uint8_t(MEM_B(-1,output))==0x5A && uint8_t(MEM_B(2048,output))==0xA5,"EEPROM read exceeded buffer");
        context.r5=255;context.r6=output;
        osEepromRead_recomp(rdram,&context);check(context.r2==0,"last block read failed");
        for(int i=0;i<8;++i)check(uint8_t(MEM_B(i,output))==expected[2040+i],"block255 aliased/wrapped");
        // Synchronous actual backing-file commit/reload. Worker drain ordering is
        // a separate existing regression and is not claimed by this fixture.
        update_save_file();
        std::ifstream file(save_context.save_file_path,std::ios::binary);
        std::vector<uint8_t> disk{std::istreambuf_iterator<char>(file),{}};
        check(disk==std::vector<uint8_t>(expected.begin(),expected.end()),"persisted EEPROM file is not exact2048bytes");
        std::fill(save_context.save_buffer.begin(),save_context.save_buffer.end(),char(0xCC));
        read_save_file();
        context.r5=0;context.r6=output;context.r7=2048;
        osEepromLongRead_recomp(rdram,&context);
        for(size_t i=0;i<expected.size();++i)check(uint8_t(MEM_B(i,output))==expected[i],"backing-file reload mismatch");
        std::cout<<"PASS real EEPROM wrappers: probe2, capacity2048, first/last blocks, all8block-address bits, long spans, word-swapped bytes, exact disk roundtrip\n";
        std::cout<<"SCOPE: disposable fixture configured Eep16k; no guest startup/save-slot acceptance; no invalid-range rejection claim under NDEBUG\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}
}
