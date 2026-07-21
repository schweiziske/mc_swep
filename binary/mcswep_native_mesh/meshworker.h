#pragma once

#include "meshbuild.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

// meshworker —— M3 内部工作线程池。
// worker 只消费不可变 SectionSnapshot 并产出 CPU Vert；不触碰 Lua、worldstate、
// materialsystem 或 IMesh。所有公开函数均由主线程调用，除 WorkerMain 内部队列操作。

namespace mcmesh::meshworker {

    constexpr int kWorkerFallbackCount = 8;
    constexpr size_t kMinInFlight = 8;
    constexpr size_t kMaxSectionResultBytes = 32u * 1024u * 1024u;
    constexpr size_t kMaxInFlightBytes = 128u * 1024u * 1024u;
    constexpr bool kWorkerCreatesMeshes = false;

    struct Job {
        uint64_t chunkKey = 0;
        int section = 0;
        uint64_t generation = 0;
        meshbuild::SectionSnapshot snapshot;
    };

    struct Result {
        uint64_t chunkKey = 0;
        int section = 0;
        uint64_t generation = 0;
        bool ok = true;
        bool meshesReady = false;
        bool meshCreateFailed = false;
        size_t resultBytes = 0;
        double buildUs = 0.0;
        double vertexBuildUs = 0.0;
        double meshStageUs = 0.0;
        meshbuild::SectionBuild build;
        meshbuild::SectionMeshes stagedMeshes;
    };

    struct Stats {
        size_t queuedJobs = 0;
        size_t activeJobs = 0;
        size_t queuedResults = 0;
        size_t outstanding = 0;
        size_t resultBytes = 0;
        uint64_t jobsEnqueued = 0;
        uint64_t jobsDropped = 0;
        int workerCount = 0;
        size_t maxInFlight = 0;
        bool workerCreatesMeshes = kWorkerCreatesMeshes;
    };

    bool Start();
    void Stop();

    // 队列已满或线程池不可用时返回 false；调用方必须保留 dirty bit。
    bool TryEnqueue(Job&& job);
    void CollectResults(std::deque<Result>& out) noexcept;
    void ReleaseResult(size_t resultBytes = 0);

    // 清除尚未开始的 job / 尚未收割的 result，并计入 jobsDropped。
    // 已在 worker 中运行的 job 由 generation 校验在主线程丢弃。
    // 返回被移除且不再可能产出结果的数量，供主线程修正 outstanding。
    size_t DiscardChunk(uint64_t chunkKey);
    size_t DiscardAll();
    void RecordStaleDrop();

    Stats GetStats();
}
