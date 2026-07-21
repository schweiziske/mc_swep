#include "pch.h"
#include "meshworker.h"

#include "tier0/platform.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace {
    std::mutex g_jobMutex;
    std::condition_variable g_jobCv;
    std::deque<mcmesh::meshworker::Job> g_jobs;

    std::mutex g_resultMutex;
    std::deque<mcmesh::meshworker::Result> g_results;

    std::vector<std::thread> g_threads;
    bool g_running = false;
    bool g_stop = false;
    size_t g_activeJobs = 0;
    uint64_t g_jobsEnqueued = 0;
    uint64_t g_jobsDropped = 0;
    size_t g_outstanding = 0;
    size_t g_resultBytes = 0;
    size_t g_maxInFlight = mcmesh::meshworker::kMinInFlight;

    void WorkerMain() {
        for (;;) {
            mcmesh::meshworker::Job job;
            {
                std::unique_lock<std::mutex> lock(g_jobMutex);
                g_jobCv.wait(lock, [] { return g_stop || !g_jobs.empty(); });
                if (g_stop) return;
                job = std::move(g_jobs.front());
                g_jobs.pop_front();
                ++g_activeJobs;
            }

            mcmesh::meshworker::Result result;
            result.chunkKey = job.chunkKey;
            result.section = job.section;
            result.generation = job.generation;
            const double t0 = Plat_FloatTime();
            try {
                const double vertexT0 = Plat_FloatTime();
                result.ok = mcmesh::meshbuild::BuildSectionVerts(job.snapshot, result.build);
                result.vertexBuildUs = (Plat_FloatTime() - vertexT0) * 1e6;
                const size_t cpuBytes = (result.build.opaque.vertices + result.build.translucent.vertices)
                    * sizeof(mcmesh::meshbuild::Vert);
                if (result.ok && cpuBytes <= mcmesh::meshworker::kMaxSectionResultBytes) {
                    if constexpr (mcmesh::meshworker::kWorkerCreatesMeshes) {
                        const double stageT0 = Plat_FloatTime();
                        result.meshesReady = mcmesh::meshbuild::StageSectionMeshes(result.build, result.stagedMeshes);
                        result.meshStageUs = (Plat_FloatTime() - stageT0) * 1e6;
                        if (!result.meshesReady) {
                            result.ok = false;
                            result.meshCreateFailed = true;
                        }
                        result.build = mcmesh::meshbuild::SectionBuild{};
                    }
                    else result.resultBytes = cpuBytes;
                }
                else if (result.ok) {
                    result.ok = false;
                }
            }
            catch (...) {
                result.ok = false;
                mcmesh::meshbuild::DestroyStagedSectionMeshes(result.stagedMeshes);
                result.build = mcmesh::meshbuild::SectionBuild{};
                result.build.ok = false;
            }
            result.buildUs = (Plat_FloatTime() - t0) * 1e6;
            bool keep = true;
            {
                std::lock_guard<std::mutex> lock(g_jobMutex);
                --g_activeJobs;
                if (keep && g_resultBytes <= mcmesh::meshworker::kMaxInFlightBytes - result.resultBytes) g_resultBytes += result.resultBytes;
                else { keep=false; result.ok=false; result.build.ok=false; result.build=mcmesh::meshbuild::SectionBuild{}; result.build.ok=false; result.resultBytes=0; }
            }
            bool queued = false;
            try {
                std::lock_guard<std::mutex> lock(g_resultMutex);
                g_results.push_back(std::move(result));
                queued = true;
            }
            catch (...) {
                // A queue allocation failure cannot be represented as a Result.
                // Release the reservation directly; normal/oversized builds are
                // always queued (oversized builds carry resultBytes == 0).
                {
                    std::lock_guard<std::mutex> lock(g_jobMutex);
                    if (g_outstanding != 0) --g_outstanding;
                    g_resultBytes = result.resultBytes > g_resultBytes ? 0 : g_resultBytes - result.resultBytes;
                    ++g_jobsDropped;
                }
                mcmesh::meshbuild::DestroyStagedSectionMeshes(result.stagedMeshes);
            }
            (void)queued;
        }
    }
}

namespace mcmesh::meshworker {

    bool Start() {
        const unsigned int detected = std::thread::hardware_concurrency();
        const size_t workerCount = detected != 0
            ? std::max<size_t>(1, size_t(detected) / 2)
            : size_t(kWorkerFallbackCount);
        {
            std::lock_guard<std::mutex> lock(g_jobMutex);
            if (g_running) return true;
            g_stop = false;
            g_maxInFlight = std::max(kMinInFlight, workerCount);
        }

        try {
            g_threads.reserve(workerCount);
            for (size_t i = 0; i < workerCount; ++i)
                g_threads.emplace_back(WorkerMain);
        }
        catch (...) {
            {
                std::lock_guard<std::mutex> lock(g_jobMutex);
                g_stop = true;
            }
            g_jobCv.notify_all();
            for (std::thread& thread : g_threads)
                if (thread.joinable()) thread.join();
            g_threads.clear();
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(g_jobMutex);
            g_running = true;
        }
        return true;
    }

    void Stop() {
        {
            std::lock_guard<std::mutex> lock(g_jobMutex);
            if (!g_running && g_threads.empty()) {
                g_jobs.clear();
                g_activeJobs = 0;
            }
            g_stop = true;
        }
        g_jobCv.notify_all();

        for (std::thread& thread : g_threads)
            if (thread.joinable()) thread.join();
        g_threads.clear();

        {
            std::lock_guard<std::mutex> lock(g_jobMutex);
            const size_t queuedDrops = g_jobs.size();
            g_jobsDropped += (uint64_t)queuedDrops;
            g_outstanding = queuedDrops > g_outstanding ? 0 : g_outstanding - queuedDrops;
            g_jobs.clear();
            g_activeJobs = 0;
            g_running = false;
        }
        uint64_t resultDrops = 0;
        {
            std::lock_guard<std::mutex> lock(g_resultMutex);
            resultDrops = (uint64_t)g_results.size();
            for (Result& result : g_results)
                mcmesh::meshbuild::DestroyStagedSectionMeshes(result.stagedMeshes);
            g_results.clear();
        }
        if (resultDrops != 0) {
            std::lock_guard<std::mutex> lock(g_jobMutex);
            g_outstanding = resultDrops > g_outstanding ? 0 : g_outstanding - (size_t)resultDrops;
            g_jobsDropped += resultDrops;
        }
        {
            // Stop 的语义是丢弃所有内部与已收割但未落地的结果；后者由
            // meshbuild 紧接着清空，因此这里最终归零在途计数。
            std::lock_guard<std::mutex> lock(g_jobMutex);
            g_outstanding = 0;
            g_resultBytes = 0;
        }
    }

    bool TryEnqueue(Job&& job) {
        {
            std::lock_guard<std::mutex> lock(g_jobMutex);
            if (!g_running || g_stop) return false;
            if (g_outstanding >= g_maxInFlight) return false;
            g_jobs.push_back(std::move(job));
            ++g_jobsEnqueued;
            ++g_outstanding;
        }
        g_jobCv.notify_one();
        return true;
    }

    void CollectResults(std::deque<Result>& out) noexcept {
        std::lock_guard<std::mutex> lock(g_resultMutex);
        if (out.empty()) out.swap(g_results);
    }

    void ReleaseResult(size_t resultBytes) {
        std::lock_guard<std::mutex> lock(g_jobMutex);
        if (g_outstanding != 0) --g_outstanding;
        g_resultBytes = resultBytes > g_resultBytes ? 0 : g_resultBytes - resultBytes;
    }

    size_t DiscardChunk(uint64_t chunkKey) {
        uint64_t dropped = 0;
        {
            std::lock_guard<std::mutex> lock(g_jobMutex);
            const size_t before = g_jobs.size();
            g_jobs.erase(std::remove_if(g_jobs.begin(), g_jobs.end(),
                [chunkKey](const Job& job) { return job.chunkKey == chunkKey; }), g_jobs.end());
            const size_t removed = before - g_jobs.size();
            dropped += (uint64_t)removed;
            g_jobsDropped += dropped;
        }
        uint64_t resultDropped = 0; size_t removedBytes = 0;
        {
            std::lock_guard<std::mutex> lock(g_resultMutex);
            const size_t before = g_results.size();
            g_results.erase(std::remove_if(g_results.begin(), g_results.end(),
                [chunkKey,&removedBytes](Result& result) {
                    if(result.chunkKey!=chunkKey)return false;
                    removedBytes+=result.resultBytes;
                    mcmesh::meshbuild::DestroyStagedSectionMeshes(result.stagedMeshes);
                    return true;
                }), g_results.end());
            resultDropped = (uint64_t)(before - g_results.size());
        }
        if (resultDropped != 0 || removedBytes != 0) {
            std::lock_guard<std::mutex> lock(g_jobMutex);
            g_resultBytes=removedBytes>g_resultBytes?0:g_resultBytes-removedBytes;
            g_jobsDropped += resultDropped;
        }
        return (size_t)dropped + (size_t)resultDropped;
    }

    size_t DiscardAll() {
        size_t queuedDrops = 0;
        {
            std::lock_guard<std::mutex> lock(g_jobMutex);
            queuedDrops = g_jobs.size();
            g_jobsDropped += (uint64_t)queuedDrops;
            g_jobs.clear();
        }
        size_t resultDrops = 0, removedBytes = 0;
        {
            std::lock_guard<std::mutex> lock(g_resultMutex);
            resultDrops = g_results.size();
            for(auto& r:g_results){removedBytes+=r.resultBytes;mcmesh::meshbuild::DestroyStagedSectionMeshes(r.stagedMeshes);}
            g_results.clear();
        }
        if (resultDrops != 0 || removedBytes != 0) {
            std::lock_guard<std::mutex> lock(g_jobMutex);
            g_resultBytes=removedBytes>g_resultBytes?0:g_resultBytes-removedBytes;
            g_jobsDropped += (uint64_t)resultDrops;
        }
        return queuedDrops + resultDrops;
    }

    void RecordStaleDrop() {
        std::lock_guard<std::mutex> lock(g_jobMutex);
        ++g_jobsDropped;
    }

    Stats GetStats() {
        Stats stats;
        {
            std::lock_guard<std::mutex> lock(g_jobMutex);
            stats.queuedJobs = g_jobs.size();
            stats.activeJobs = g_activeJobs;
            stats.outstanding = g_outstanding;
            stats.resultBytes = g_resultBytes;
            stats.jobsEnqueued = g_jobsEnqueued;
            stats.jobsDropped = g_jobsDropped;
            stats.workerCount = g_running ? (int)g_threads.size() : 0;
            stats.maxInFlight = g_maxInFlight;
            stats.workerCreatesMeshes = kWorkerCreatesMeshes;
        }
        {
            std::lock_guard<std::mutex> lock(g_resultMutex);
            stats.queuedResults = g_results.size();
        }
        return stats;
    }
}
