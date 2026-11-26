// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Pi's CPU self-test (platform/common/cpu_selftest), run here with the
// generated code on the A64 simulator: it must pass, and it must notice a
// broken CPU.

#include "platform/common/cpu_selftest.h"
#include "core/cpu/ppc750/ppc750.h"
#include "core/cpu/ppc750/jit_worker.h"
#include "tests/framework/g3test.h"
#include "tools/a64sim/a64_sim.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace g3;

namespace {
u64 now_us(void*)
{
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
struct Rig {
    std::vector<u8> work = std::vector<u8>(CpuSelfTest::kWorkBytes);
    std::vector<u32> code = std::vector<u32>(128 * 1024);
    const void* range[2] = {code.data(), code.data() + code.size()};
    CpuSelfTest::Setup setup()
    {
        CpuSelfTest::Setup s;
        s.work = work.data();
        s.code = code.data();
        s.code_words = static_cast<u32>(code.size());
#if !defined(__aarch64__)
        s.runner = a64::simulate_jit_block;
        s.runner_context = range;
#endif
        s.clock_us = now_us;
        s.programs = 20;
        s.bench_instructions = 2000000;
        return s;
    }
};
} // namespace

TEST_CASE("platform/selftest/passes_and_measures")
{
    Rig r;
    const CpuSelfTest::Result res = CpuSelfTest::run(r.setup());
    CHECK(res.ok());
    CHECK(res.jit);
    CHECK_EQ(res.programs, 20u);
    CHECK(res.instructions > 100000);
    CHECK(res.jit_block_runs > 1000);
    CHECK(res.mips_x100[0] > 0);
    CHECK(res.mips_x100[1] > 0);
    CHECK(res.mips_x100[2] > 0);
    char l1[160], l2[160];
    CpuSelfTest::describe(res, l1, sizeof l1, l2, sizeof l2);
    std::printf("    %s\n    %s (generated code on the simulator here)\n", l1, l2);
    CHECK(std::strncmp(l1, "CPU self-test: PASS, 20/20 programs identical with generated code", 64) == 0);
    CHECK(std::strncmp(l2, "MIPS: interpreter ", 18) == 0);
}

TEST_CASE("platform/selftest/without_generated_code")
{
    Rig r;
    CpuSelfTest::Setup s = r.setup();
    s.code = nullptr;
    s.programs = 4;
    const CpuSelfTest::Result res = CpuSelfTest::run(s);
    CHECK(res.ok());
    CHECK(!res.jit);
    CHECK_EQ(res.mips_x100[2], 0u);
    char l1[160], l2[160];
    CpuSelfTest::describe(res, l1, sizeof l1, l2, sizeof l2);
    CHECK(std::strstr(l1, "block cache only") != nullptr);
    CHECK(std::strstr(l2, "generated code -") != nullptr);
}

namespace {
// A runner that runs the generated code and then breaks the CPU state a
// little (one GPR bit) once in a while: the self-test must report it.
struct Breaker {
    const void* range[2];
    u32 calls = 0;
};
void breaking_runner(void* code, void* cpu, void* ctx)
{
    auto* b = static_cast<Breaker*>(ctx);
#if defined(__aarch64__)
    reinterpret_cast<void (*)(void*)>(code)(cpu);
#else
    a64::simulate_jit_block(code, cpu, b->range);
#endif
    if (++b->calls % 5000 == 0)
        static_cast<ppc::Ppc750*>(cpu)->state().gpr[3] ^= 1;
}
} // namespace

TEST_CASE("platform/selftest/notices_a_broken_cpu")
{
    Rig r;
    Breaker br{{r.code.data(), r.code.data() + r.code.size()}};
    CpuSelfTest::Setup s = r.setup();
    s.runner = breaking_runner;
    s.runner_context = &br;
    s.clock_us = nullptr;  // correctness only
    const CpuSelfTest::Result res = CpuSelfTest::run(s);
    CHECK(!res.ok());
    CHECK(res.failed > 0);
    CHECK_EQ(res.mips_x100[0], 0u);  // speed part skipped without a clock
    char l1[160], l2[160];
    CpuSelfTest::describe(res, l1, sizeof l1, l2, sizeof l2);
    std::printf("    %s\n", l1);
    CHECK(std::strncmp(l1, "CPU self-test: FAIL", 19) == 0);
    CHECK(std::strstr(l1, "differs") != nullptr);
}

namespace {
// Stands in for the Pi's core 2: a thread polling whichever worker is attached.
struct WorkerThread {
    std::atomic<void*> worker{nullptr};
    std::atomic<int> busy{0};
    std::atomic<bool> stop{false};
    u32 attached = 0;
    std::thread t{[this] {
        while (!stop.load()) {
            busy.store(1);
            if (void* w = worker.load())
                static_cast<ppc::JitWorker*>(w)->poll();
            busy.store(0);
            std::this_thread::yield();
        }
    }};
    static void attach(void* ctx, void* w)
    {
        auto* self = static_cast<WorkerThread*>(ctx);
        self->worker.store(w);
        if (w) {
            ++self->attached;
        } else {
            while (self->busy.load())
                std::this_thread::yield();
        }
    }
    ~WorkerThread()
    {
        stop.store(true);
        t.join();
    }
};
} // namespace

TEST_CASE("platform/selftest/background_compilation")
{
    Rig r;
    WorkerThread wt;
    CpuSelfTest::Setup s = r.setup();
    s.worker_attach = WorkerThread::attach;
    s.worker_context = &wt;
    s.clock_us = nullptr;  // correctness only
    const CpuSelfTest::Result res = CpuSelfTest::run(s);
    CHECK(res.ok());
    CHECK(res.jit);
    CHECK(res.background);
    CHECK_EQ(wt.attached, 20u);           // one worker per program
    CHECK(wt.worker.load() == nullptr);   // detached at the end
    CHECK(res.installed > 100);           // code really came from the other thread
    char l1[160], l2[160];
    CpuSelfTest::describe(res, l1, sizeof l1, l2, sizeof l2);
    std::printf("    %s (%llu blocks compiled on the other thread)\n", l1,
                static_cast<unsigned long long>(res.installed));
    CHECK(std::strstr(l1, "PASS, 20/20") != nullptr);
    CHECK(std::strstr(l1, "another core") != nullptr);
}
