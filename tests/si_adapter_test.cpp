#include "si_adapter.hpp"
#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool condition, const char* why) { if (!condition) throw std::runtime_error(why); }
template<class F> void rejected(F&& fn) {
    bool caught = false;
    try { fn(); } catch (const std::runtime_error&) { caught = true; }
    require(caught, "invalid transaction was accepted");
}
tooie::si::Challenge hex(const std::string& text) {
    require(text.size() == 30, "bad reference vector length");
    tooie::si::Challenge value{};
    for (size_t i = 0; i < value.size(); ++i) value[i] = std::stoul(text.substr(i * 2, 2), nullptr, 16);
    return value;
}
}

int main(int argc, char** argv) {
    try {
        require(argc == 2, "pass the independent X-Scale reference vector file");
        std::ifstream vectors(argv[1]);
        require(bool(vectors), "cannot read reference vectors");
        unsigned count = 0;
        std::string challenge, response;
        while (vectors >> challenge >> response) {
            auto input = hex(challenge), expected = hex(response);
            require(tooie::si::cic6105_response(input) == expected, "CIC reference mismatch");
            for (uint32_t address : {0x80000100u, 0xA0000100u, 0x800000C0u}) {
                std::vector<uint8_t> ram(512, 0xA5);
                const uint32_t offset = address & 0x1FFFFFFFu;
                const auto put = [&](size_t index, uint8_t value) { ram[(offset + index) ^ 3] = value; };
                const auto get = [&](size_t index) { return ram[(offset + index) ^ 3]; };
                for (size_t i = 0; i < 46; ++i) put(i, 0xFF);
                put(46, 15); put(47, 15);
                for (size_t i = 0; i < 15; ++i) put(48 + i, input[i]);
                put(63, 2);
                const auto written = ram;
                tooie::si::Device device;
                device.transfer(ram, 1, address);
                require(ram == written, "OS_WRITE changed guest source");
                // Prove the device owns its PIF data: overwrite all guest bytes before reading.
                for (size_t i = 0; i < 64; ++i) put(i, 0xCD);
                device.transfer(ram, 0, address);
                for (size_t i = 0; i < 46; ++i) require(get(i) == 0xFF, "PIF prefix changed");
                require(get(46) == 0 && get(47) == 0 && get(63) == 0, "PIF result control bytes wrong");
                for (size_t i = 0; i < 15; ++i) require(get(48 + i) == expected[i], "DMA byte/nibble order mismatch");
                for (size_t i = 0; i < ram.size(); ++i)
                    if (i < offset || i >= offset + 64) require(ram[i] == written[i], "DMA damaged guard bytes");
                const auto read = ram;
                device.transfer(ram, 0, address);
                require(ram == read, "repeat read recomputed challenge");
                rejected([&] { device.transfer(ram, 2, address); });
                rejected([&] { device.transfer(ram, 0, address + 1); });
                rejected([&] { device.transfer(ram, 0, 0xA4800000); });
                rejected([&] { device.transfer(ram, 0, 0x80000200); });
                rejected([&] { device.transfer(ram, 0, 0x800001FC); });
                // Unsupported controller processing is visible, leaves last PIF result intact.
                put(63, 1);
                rejected([&] { device.transfer(ram, 1, address); });
                device.transfer(ram, 0, address);
                require(ram == read, "rejected write mutated PIF state");
                for (size_t invalid_index : {size_t(0), size_t(46), size_t(47)}) {
                    ram = written;
                    put(invalid_index, 0);
                    rejected([&] { device.transfer(ram, 1, address); });
                    device.transfer(ram, 0, address);
                    require(ram == read, "malformed challenge mutated PIF state");
                }
                device.reset();
                rejected([&] { device.transfer(ram, 0, address); });
            }
            ++count;
        }
        require(count >= 256, "insufficient independent vector coverage");
        std::cout << "PASS algorithm_vectors=" << count << " dma_transactions=" << count * 3
                  << " (cached/uncached, write/read/repeat, guards and rejection checks)\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
