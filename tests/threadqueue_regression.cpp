// Exercise the actual ultramodern queue implementation and guest pointer layout.
#include "ultramodern/ultramodern.hpp"

#include <array>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace {
constexpr int32_t guest_queue = static_cast<int32_t>(0x80000100u);
constexpr std::array<int32_t, 5> nodes = {
    static_cast<int32_t>(0x80001000u), static_cast<int32_t>(0x80001200u),
    static_cast<int32_t>(0x80001400u), static_cast<int32_t>(0x80001600u),
    static_cast<int32_t>(0x80001800u)};
void require(bool value, const char* description) {
    if (!value) throw std::runtime_error(description);
}
struct Fixture {
    static constexpr size_t reservation_size = size_t{4} * 1024 * 1024 * 1024;
    static constexpr size_t accessible_size = 8 * 1024 * 1024;
    // Match the native host contract: NULLPTR's host translation (+2 GiB)
    // must stay inaccessible instead of being masked by zero-filled memory.
    uint8_t* rdram = nullptr;
    int32_t queue;
    explicit Fixture(int32_t q) : queue(q) {
#ifdef _WIN32
        rdram = static_cast<uint8_t*>(VirtualAlloc(nullptr,reservation_size,MEM_RESERVE,PAGE_NOACCESS));
        require(rdram != nullptr,"guest address reservation failed");
        if (!VirtualAlloc(rdram,accessible_size,MEM_COMMIT,PAGE_READWRITE)) {
            VirtualFree(rdram,0,MEM_RELEASE);
            throw std::runtime_error("guest memory commit failed");
        }
#else
        auto* region=mmap(nullptr,reservation_size,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        require(region != MAP_FAILED,"guest address reservation failed");
        rdram=static_cast<uint8_t*>(region);
        if (mprotect(rdram,accessible_size,PROT_READ|PROT_WRITE)!=0) {
            munmap(rdram,reservation_size);
            throw std::runtime_error("guest memory commit failed");
        }
#endif
        require(ultramodern::thread_queue_empty(rdram, queue), "fixture queue not empty");
        for (size_t i = 0; i < nodes.size(); ++i) {
            auto* t = TO_PTR(OSThread, nodes[i]);
            t->id = static_cast<int>(i + 1);
            t->priority = static_cast<int>(40 - i * 10);
        }
    }
    ~Fixture() {
#ifdef _WIN32
        VirtualFree(rdram,0,MEM_RELEASE);
#else
        munmap(rdram,reservation_size);
#endif
    }
    OSThread* node(size_t index) { return TO_PTR(OSThread, nodes[index]); }
    void insert(size_t index) { ultramodern::thread_queue_insert(rdram, queue, nodes[index]); }
    bool remove(size_t index) { return ultramodern::thread_queue_remove(rdram, queue, nodes[index]); }
    void populate() { insert(2); insert(0); insert(3); insert(1); }
    void expect(std::initializer_list<size_t> indices) {
        auto cur = ultramodern::thread_queue_peek(rdram, queue);
        for (size_t index : indices) {
            require(cur == nodes[index], "wrong queue order or link");
            require(node(index)->queue == queue, "remaining node queue metadata changed");
            cur = node(index)->next;
        }
        require(cur == NULLPTR, "unexpected extra node or cycle");
        require(ultramodern::thread_queue_empty(rdram, queue) == (indices.size() == 0), "empty predicate mismatch");
    }
    void drain(std::initializer_list<size_t> indices) {
        expect(indices);
        for (size_t index : indices) {
            require(ultramodern::thread_queue_pop(rdram, queue) == nodes[index], "wrong pop order");
            require(node(index)->queue == NULLPTR, "pop did not clear queue field");
        }
        expect({});
    }
};
void run(int32_t queue, const std::string& test) {
    Fixture f(queue);
    if (test == "nullqueue") {
        f.populate();
        require(!ultramodern::thread_queue_remove(f.rdram,NULLPTR,nodes[0]),"null queue removal reported success");
        f.drain({0,1,2,3});
    } else if (test == "empty") {
        require(!f.remove(4), "empty removal reported success");
        f.expect({});
    } else if (test == "singleton") {
        f.insert(0);
        require(!f.remove(4), "singleton absent removal reported success");
        f.expect({0});
        require(f.remove(0), "singleton removal failed");
        f.expect({});
        require(!f.remove(0), "repeat removal reported success");
    } else if (test == "ties") {
        f.node(1)->priority = f.node(0)->priority;
        f.insert(0); f.insert(2); f.insert(1);
        f.expect({0, 1, 2}); // Original libultra FIFO among equal priorities.
        require(f.remove(1), "equal-priority middle removal failed");
        f.drain({0, 2});
    } else {
        f.populate();
        f.expect({0, 1, 2, 3});
        if (test == "head") {
            require(f.remove(0), "head removal failed"); f.drain({1, 2, 3});
        } else if (test == "middle") {
            require(f.remove(1), "middle removal failed"); f.drain({0, 2, 3});
        } else if (test == "tail") {
            require(f.remove(3), "tail removal failed"); f.drain({0, 1, 2});
        } else if (test == "absent") {
            require(!f.remove(4), "absent removal reported success"); f.drain({0, 1, 2, 3});
        } else if (test == "reprioritize") {
            require(f.remove(2), "reprioritization removal failed");
            // osSetThreadPri uses t->queue after remove: retain that existing contract.
            require(f.node(2)->queue == queue, "remove cleared queue metadata needed for reinsertion");
            f.node(2)->priority = 50;
            ultramodern::thread_queue_insert(f.rdram, f.node(2)->queue, nodes[2]);
            f.drain({2, 0, 1, 3});
        } else throw std::runtime_error("unknown test");
    }
}
}
int main(int argc, char** argv) {
    try {
        const std::vector<std::string> tests = {"nullqueue", "head", "middle", "tail", "absent", "empty", "singleton", "ties", "reprioritize"};
        size_t passed = 0;
        for (const std::string kind : {"running", "guest"}) {
            if (argc > 1 && kind != argv[1]) continue;
            for (const auto& test : tests) {
                if (argc > 2 && test != argv[2]) continue;
                std::cout << "RUN " << kind << ' ' << test << std::endl;
                run(kind == "running" ? ultramodern::running_queue : guest_queue, test);
                ++passed;
                std::cout << "PASS " << kind << ' ' << test << std::endl;
            }
        }
        require(passed != 0, "no matching cases");
        std::cout << "PASS cases=" << passed << std::endl;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << std::endl;
        return 1;
    }
}
