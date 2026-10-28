// Per-thread user-space instruction and cycle counters (perf_event_open).
// Only the calling thread is counted: the collector thread's work is excluded.
#pragma once
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cstdint>
#include <cstring>

struct ThreadCounters {
    int fdInstr = -1, fdCycles = -1;
    static int open1(uint64_t config, int group) {
        perf_event_attr a;
        std::memset(&a, 0, sizeof a);
        a.type = PERF_TYPE_HARDWARE;
        a.size = sizeof a;
        a.config = config;
        a.disabled = group == -1 ? 1 : 0;
        a.exclude_kernel = 1;
        a.exclude_hv = 1;
        a.read_format = 0;
        return (int)syscall(SYS_perf_event_open, &a, 0, -1, group, 0);
    }
    ThreadCounters() {
        fdInstr = open1(PERF_COUNT_HW_INSTRUCTIONS, -1);
        fdCycles = open1(PERF_COUNT_HW_CPU_CYCLES, fdInstr);
    }
    ~ThreadCounters() {
        if (fdInstr >= 0) close(fdInstr);
        if (fdCycles >= 0) close(fdCycles);
    }
    void start() {
        if (fdInstr < 0) return;
        ioctl(fdInstr, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP);
        ioctl(fdInstr, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
    }
    void stop(uint64_t& instr, uint64_t& cycles) {
        instr = cycles = 0;
        if (fdInstr < 0) return;
        ioctl(fdInstr, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);
        if (read(fdInstr, &instr, sizeof instr) != sizeof instr) instr = 0;
        if (read(fdCycles, &cycles, sizeof cycles) != sizeof cycles) cycles = 0;
    }
};
