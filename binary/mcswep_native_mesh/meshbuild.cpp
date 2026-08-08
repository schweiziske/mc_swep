#include "pch.h"
#include "patchdecode.h"
#include "meshbuild.h"
#include "meshworker.h"
#include "lightatlas.h"
#include "blockdefs.h"
#include "worldstate.h"
#include "blob_format.h"
#include "emitters.h"

#include "tier0/platform.h"

#include <array>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "mathlib/vmatrix.h"

using namespace GarrysMod::Lua;
using namespace mcmesh;
namespace ws = mcmesh::worldstate;

namespace {
    struct FaceCorner { float ox, oy, oz; int uf, vf; };
    struct FaceDef {
        float nx, ny, nz;
        int ox, oy, oz;
        FaceCorner c[4];
    };

    // cl_mesh_constants.lua:39-46，顺序 TOP,BOTTOM,XP,XN,YP,YN。
    static const FaceDef FACES[6] = {
        { 0,0,1,   0,0,1,   {{0,0,1,0,1},{1,0,1,1,1},{1,1,1,1,0},{0,1,1,0,0}} },
        { 0,0,-1,  0,0,-1,  {{0,1,0,0,1},{1,1,0,1,1},{1,0,0,1,0},{0,0,0,0,0}} },
        { 1,0,0,   1,0,0,   {{1,0,0,0,1},{1,1,0,1,1},{1,1,1,1,0},{1,0,1,0,0}} },
        { -1,0,0, -1,0,0,   {{0,1,0,0,1},{0,0,0,1,1},{0,0,1,1,0},{0,1,1,0,0}} },
        { 0,1,0,   0,1,0,   {{1,1,0,0,1},{0,1,0,1,1},{0,1,1,1,0},{1,1,1,0,0}} },
        { 0,-1,0,  0,-1,0,  {{0,0,0,0,1},{1,0,0,1,1},{1,0,1,1,0},{0,0,1,0,0}} },
    };

    static const int TRI[6] = { 0, 2, 1, 0, 3, 2 };

    inline void RotatedUVFlags(int uf, int vf, int rot, bool flipU, bool flipV, int& ou, int& ov) {
        rot &= 3;
        if (rot == 1) { int nu = vf, nv = 1 - uf; uf = nu; vf = nv; }
        else if (rot == 2) { uf = 1 - uf; vf = 1 - vf; }
        else if (rot == 3) { int nu = 1 - vf, nv = uf; uf = nu; vf = nv; }
        if (flipU) uf = 1 - uf;
        if (flipV) vf = 1 - vf;
        ou = uf; ov = vf;
    }

    inline void TileUV(int tile, double& u0, double& v0, double& u1, double& v1) {
        const auto& A = blockdefs::g_atlas;
        const int col = tile % A.cols;
        const int row = tile / A.cols;
        const int stride = A.stride ? A.stride : A.tile;
        const double px0 = double(col) * stride + A.pad;
        const double py0 = double(row) * stride + A.pad;
        const double inset = A.inset;
        u0 = (px0 + inset) / A.w;
        v0 = (py0 + inset) / A.h;
        u1 = (px0 + A.tile - inset) / A.w;
        v1 = (py0 + A.tile - inset) / A.h;
    }

    inline bool FaceVisible(uint16_t curId, uint16_t nbId, uint8_t nbOrient) {
        if (nbId == 0) return true;
        const blockdefs::BlockDef* nd = blockdefs::FindDef(nbId);
        if (!nd) return true;
        if (nd->flags & blockdefs::FLAG_TRANSPARENT) return nbId != curId;
        if (!blockdefs::OrientFullCube(nd, nbOrient)) return true;
        return false;
    }

    inline int PopCount64(uint64_t v) {
        int n = 0;
        while (v) { v &= v - 1; ++n; }
        return n;
    }

    inline int LowestSetBit(uint64_t mask) {
        int bit = 0;
        while ((mask & 1) == 0) { mask >>= 1; ++bit; }
        return bit;
    }

    inline bool Ready() {
        auto defsLock = blockdefs::LockForRead();
        return g_promise.SS > 0 && blockdefs::g_loaded;
    }

    using GenerationArray = std::array<uint64_t, 64>;
    std::unordered_map<uint64_t, GenerationArray> g_generations;
    uint64_t g_nextGeneration = 0;
    std::deque<mcmesh::meshworker::Result> g_readyResults;
    std::unordered_set<uint64_t> g_hiddenChunks;

    struct NativeDrawView {
        double eye[3]{}, forward[3]{}, right[3]{}, up[3]{};
        double tanX = 0, tanY = 0, offset[3]{}, distance = 0;
        double lodStartChunks = 24.0, lodStartDistance = 0;
        uint64_t occlusionEpoch = 0;
        bool frustum = true, horizontalDistance = false, collectKeys = false, occlusionCull = false, lod = false;
    };

    struct NativeDrawResult {
        uint64_t testedChunks = 0, drawnChunks = 0, distanceCulled = 0, frustumCulled = 0;
        uint64_t testedSections = 0, visibleChunks = 0, visibleSections = 0;
        uint64_t drawnSections = 0, meshesDrawn = 0, verticesCandidate = 0, verticesDrawn = 0;
        uint64_t verticesFullDetail = 0, verticesLodCulled = 0, verticesDirectionCulled = 0, trianglesDrawn = 0;
        uint64_t lodSectionsDrawn = 0, lodMeshesDrawn = 0;
        uint64_t occlusionTestedSections = 0, occlusionCulledSections = 0, occlusionCulledChunks = 0;
        uint64_t occlusionComponentSections = 0, occlusionReachableSections = 0;
        bool directionalDraw = false, occlusionCull = false, occlusionApplied = false, lod = false, lodApplied = false;
        double elapsedUs = 0, cullUs = 0, lodStartChunks = 0;
        std::string visibleKeys;
    };

    struct DrawChunkMetadata {
        uint64_t key = 0, passMask[2]{};
        int cx = 0, cy = 0, cz = 0;
        bool hidden = false;
        uint64_t occlusionReadyMask = 0, lodReadyMask = 0;
        std::array<std::array<uint8_t, 6>, 64> occlusionVisibleFrom{};
    };

    struct VisibleDrawEntry {
        uint64_t key = 0, sectionMask = 0, lodMask = 0;
        int cx = 0, cy = 0, cz = 0;
        std::array<uint64_t, 6> directionalMasks{};
    };

    struct NativeCullFrame {
        uint64_t serial = 0;
        NativeDrawView view;
        NativeDrawResult pass[2];
        std::vector<VisibleDrawEntry> visible[2];
    };

    using SectionMaskMap = std::unordered_map<uint64_t, uint64_t>;
    using SectionEntryMap = std::unordered_map<uint64_t, std::array<uint8_t, 64>>;
    using DrawMetadataLookup = std::unordered_map<uint64_t, const DrawChunkMetadata*>;
    struct SectionQueueEntry { int x = 0, y = 0, z = 0; uint8_t incoming = 0; };
    struct OcclusionTraversal {
        bool requested = false, applied = false;
        SectionMaskMap component, reachable;
        uint64_t componentSections = 0, reachableSections = 0;
    };

    constexpr int OCCLUSION_DX[6] = { 0, 0, 1, -1, 0, 0 };
    constexpr int OCCLUSION_DY[6] = { 0, 0, 0, 0, 1, -1 };
    constexpr int OCCLUSION_DZ[6] = { 1, -1, 0, 0, 0, 0 };
    constexpr int OCCLUSION_OPPOSITE[6] = { 1, 0, 3, 2, 5, 4 };
    constexpr uint8_t OCCLUSION_CAMERA_ENTRY = 0x40;

    std::mutex g_drawMetadataMutex;
    std::unordered_map<uint64_t, DrawChunkMetadata> g_drawMetadata;
    std::atomic<uint64_t> g_occlusionMetadataEpoch{ 1 };
    std::mutex g_cullMutex;
    std::condition_variable g_cullCv;
    std::thread g_cullThread;
    bool g_cullRunning = false, g_cullStop = false, g_cullPending = false;
    bool g_cullCollectPending = false, g_cullFailed = false;
    uint64_t g_cullNextSerial = 0;
    NativeDrawView g_cullRequest;
    std::shared_ptr<const NativeCullFrame> g_latestCullFrame;
    std::shared_ptr<const NativeCullFrame> g_activeCullFrame;
    std::string g_pendingVisibleKeys;

    bool ReadFiniteNumber(ILuaBase* LUA, int index, double& out) {
        if (!LUA->IsType(index, Type::Number)) return false;
        out = LUA->GetNumber(index);
        return std::isfinite(out);
    }

    void AppendI32LE(std::string& out, int value) {
        const uint32_t v = uint32_t(int32_t(value));
        out.push_back(char(v & 0xff)); out.push_back(char((v >> 8) & 0xff));
        out.push_back(char((v >> 16) & 0xff)); out.push_back(char((v >> 24) & 0xff));
    }

    bool AABBInFrustum(const NativeDrawView& view, double x, double y, double z,
        double hx, double hy, double hz) {
        if (!view.frustum) return true;
        const double dx = x - view.eye[0], dy = y - view.eye[1], dz = z - view.eye[2];
        const double normals[5][3] = {
            { view.forward[0], view.forward[1], view.forward[2] },
            { view.right[0] + view.forward[0] * view.tanX, view.right[1] + view.forward[1] * view.tanX, view.right[2] + view.forward[2] * view.tanX },
            { -view.right[0] + view.forward[0] * view.tanX, -view.right[1] + view.forward[1] * view.tanX, -view.right[2] + view.forward[2] * view.tanX },
            { view.up[0] + view.forward[0] * view.tanY, view.up[1] + view.forward[1] * view.tanY, view.up[2] + view.forward[2] * view.tanY },
            { -view.up[0] + view.forward[0] * view.tanY, -view.up[1] + view.forward[1] * view.tanY, -view.up[2] + view.forward[2] * view.tanY },
        };
        for (const auto& n : normals) {
            const double radius = hx * std::abs(n[0]) + hy * std::abs(n[1]) + hz * std::abs(n[2]);
            if (n[0] * dx + n[1] * dy + n[2] * dz < -radius) return false;
        }
        return true;
    }

    bool AABBInDistance(const NativeDrawView& view, double x, double y, double z,
        double hx, double hy, double hz) {
        if (view.distance <= 0) return true;
        const double slack = view.horizontalDistance ? std::sqrt(hx * hx + hy * hy)
            : std::sqrt(hx * hx + hy * hy + hz * hz);
        const double limit = view.distance + slack;
        const double dx = x - view.eye[0], dy = y - view.eye[1], dz = z - view.eye[2];
        return view.horizontalDistance ? dx * dx + dy * dy <= limit * limit
            : dx * dx + dy * dy + dz * dz <= limit * limit;
    }

    double DistanceSquaredToAABB(const NativeDrawView& view,
        double minX, double minY, double minZ, double maxX, double maxY, double maxZ) {
        const double dx = view.eye[0] < minX ? minX - view.eye[0]
            : view.eye[0] > maxX ? view.eye[0] - maxX : 0.0;
        const double dy = view.eye[1] < minY ? minY - view.eye[1]
            : view.eye[1] > maxY ? view.eye[1] - maxY : 0.0;
        const double dz = view.eye[2] < minZ ? minZ - view.eye[2]
            : view.eye[2] > maxZ ? view.eye[2] - maxZ : 0.0;
        return dx * dx + dy * dy + dz * dz;
    }

    int FloorDiv(int value, int divisor) {
        int quotient = value / divisor;
        if (value % divisor < 0) --quotient;
        return quotient;
    }

    bool ResolveSection(const DrawMetadataLookup& lookup, int x, int y, int z,
        const DrawChunkMetadata*& metadata, int& section) {
        if (g_promise.NSX <= 0 || g_promise.NSZ <= 0) return false;
        const int cx = FloorDiv(x, g_promise.NSX);
        const int cy = FloorDiv(y, g_promise.NSX);
        const int cz = FloorDiv(z, g_promise.NSZ);
        if (cx < -1000000 || cx > 1000000 || cy < -1000000 || cy > 1000000
            || cz < -1000000 || cz > 1000000) return false;
        const auto found = lookup.find(ws::PackChunkKey(cx, cy, cz));
        if (found == lookup.end()) return false;
        const int sx = x - cx * g_promise.NSX;
        const int sy = y - cy * g_promise.NSX;
        const int sz = z - cz * g_promise.NSZ;
        section = sx + sy * g_promise.NSX + sz * g_promise.NSX * g_promise.NSX;
        if (section < 0 || section >= g_promise.SPC || section >= 64) return false;
        metadata = found->second;
        return metadata != nullptr;
    }

    bool SectionInCullBounds(const NativeDrawView& view, int x, int y, int z) {
        const double size = g_promise.SS * g_promise.BS;
        const double half = size * 0.5;
        const double centerX = x * size + view.offset[0] + half;
        const double centerY = y * size + view.offset[1] + half;
        const double centerZ = z * size + view.offset[2] + half;
        return AABBInDistance(view, centerX, centerY, centerZ, half, half, half)
            && AABBInFrustum(view, centerX, centerY, centerZ, half, half, half);
    }

    bool MarkSection(SectionMaskMap& sections, uint64_t key, int section) {
        const uint64_t bit = 1ull << section;
        uint64_t& mask = sections[key];
        if (mask & bit) return false;
        mask |= bit;
        return true;
    }

    bool HasSection(const SectionMaskMap& sections, uint64_t key, int section) {
        const auto found = sections.find(key);
        return found != sections.end() && (found->second & (1ull << section)) != 0;
    }

    OcclusionTraversal BuildOcclusionTraversal(const NativeDrawView& view,
        const std::vector<DrawChunkMetadata>& metadataSnapshot) {
        OcclusionTraversal traversal;
        traversal.requested = view.occlusionCull;
        if (!view.occlusionCull || metadataSnapshot.empty() || g_promise.SS <= 0) return traversal;

        DrawMetadataLookup lookup;
        lookup.reserve(metadataSnapshot.size() * 2 + 1);
        for (const DrawChunkMetadata& metadata : metadataSnapshot) lookup.emplace(metadata.key, &metadata);

        const double sectionSize = g_promise.SS * g_promise.BS;
        const double localX = (view.eye[0] - view.offset[0]) / sectionSize;
        const double localY = (view.eye[1] - view.offset[1]) / sectionSize;
        const double localZ = (view.eye[2] - view.offset[2]) / sectionSize;
        constexpr double minCoord = double(std::numeric_limits<int>::min()) + 2.0;
        constexpr double maxCoord = double(std::numeric_limits<int>::max()) - 2.0;
        if (localX < minCoord || localX > maxCoord || localY < minCoord || localY > maxCoord
            || localZ < minCoord || localZ > maxCoord) return traversal;
        const int cameraX = int(std::floor(localX));
        const int cameraY = int(std::floor(localY));
        const int cameraZ = int(std::floor(localZ));

        const DrawChunkMetadata* cameraMetadata = nullptr;
        int cameraSection = 0;
        if (!ResolveSection(lookup, cameraX, cameraY, cameraZ, cameraMetadata, cameraSection)) return traversal;

        traversal.component.reserve(metadataSnapshot.size() * 2 + 1);
        std::vector<SectionQueueEntry> queue;
        queue.reserve(metadataSnapshot.size() * 4 + 16);
        MarkSection(traversal.component, cameraMetadata->key, cameraSection);
        ++traversal.componentSections;
        queue.push_back({ cameraX, cameraY, cameraZ, 0 });
        for (size_t head = 0; head < queue.size(); ++head) {
            const SectionQueueEntry current = queue[head];
            for (int direction = 0; direction < 6; ++direction) {
                const int nx = current.x + OCCLUSION_DX[direction];
                const int ny = current.y + OCCLUSION_DY[direction];
                const int nz = current.z + OCCLUSION_DZ[direction];
                if (!SectionInCullBounds(view, nx, ny, nz)) continue;
                const DrawChunkMetadata* neighborMetadata = nullptr;
                int neighborSection = 0;
                if (!ResolveSection(lookup, nx, ny, nz, neighborMetadata, neighborSection)) continue;
                if (!MarkSection(traversal.component, neighborMetadata->key, neighborSection)) continue;
                ++traversal.componentSections;
                queue.push_back({ nx, ny, nz, 0 });
            }
        }

        SectionEntryMap entries;
        entries.reserve(traversal.component.size() * 2 + 1);
        traversal.reachable.reserve(traversal.component.size() * 2 + 1);
        queue.clear();
        queue.push_back({ cameraX, cameraY, cameraZ, OCCLUSION_CAMERA_ENTRY });
        for (size_t head = 0; head < queue.size(); ++head) {
            const SectionQueueEntry current = queue[head];
            const DrawChunkMetadata* metadata = nullptr;
            int section = 0;
            if (!ResolveSection(lookup, current.x, current.y, current.z, metadata, section)
                || !HasSection(traversal.component, metadata->key, section)) continue;

            uint8_t& seenEntries = entries[metadata->key][section];
            const uint8_t freshEntries = uint8_t(current.incoming & ~seenEntries);
            if (!freshEntries) continue;
            seenEntries = uint8_t(seenEntries | freshEntries);
            if (MarkSection(traversal.reachable, metadata->key, section)) ++traversal.reachableSections;

            uint8_t exits = (freshEntries & OCCLUSION_CAMERA_ENTRY) ? mcmesh::meshbuild::kAllSectionFaces : 0;
            const bool ready = (metadata->occlusionReadyMask & (1ull << section)) != 0;
            for (int entry = 0; entry < 6; ++entry) {
                if ((freshEntries & (1u << entry)) == 0) continue;
                exits = uint8_t(exits | (ready
                    ? metadata->occlusionVisibleFrom[section][entry]
                    : mcmesh::meshbuild::kAllSectionFaces));
            }
            for (int direction = 0; direction < 6; ++direction) {
                if ((exits & (1u << direction)) == 0) continue;
                const int nx = current.x + OCCLUSION_DX[direction];
                const int ny = current.y + OCCLUSION_DY[direction];
                const int nz = current.z + OCCLUSION_DZ[direction];
                const DrawChunkMetadata* neighborMetadata = nullptr;
                int neighborSection = 0;
                if (!ResolveSection(lookup, nx, ny, nz, neighborMetadata, neighborSection)
                    || !HasSection(traversal.component, neighborMetadata->key, neighborSection)) continue;
                queue.push_back({ nx, ny, nz, uint8_t(1u << OCCLUSION_OPPOSITE[direction]) });
            }
        }

        traversal.applied = true;
        return traversal;
    }

    void RefreshDrawMetadata(uint64_t key) {
        DrawChunkMetadata metadata;
        metadata.key = key;
        int cx, cy, cz; ws::UnpackChunkKey(key, cx, cy, cz);
        metadata.cx = cx; metadata.cy = cy; metadata.cz = cz;
        metadata.hidden = g_hiddenChunks.find(key) != g_hiddenChunks.end();
        auto it = mcmesh::meshbuild::g_meshes.find(key);
        if (it != mcmesh::meshbuild::g_meshes.end()) {
            for (int sec = 0; sec < g_promise.SPC; ++sec) {
                const mcmesh::meshbuild::SectionMeshes& section = it->second.section[sec];
                if (!section.opaque.empty()) metadata.passMask[0] |= 1ull << sec;
                if (!section.translucent.empty()) metadata.passMask[1] |= 1ull << sec;
                if (section.lodReady && !section.lodOpaque.empty()) metadata.lodReadyMask |= 1ull << sec;
                if (section.occlusion.ready) {
                    metadata.occlusionReadyMask |= 1ull << sec;
                    metadata.occlusionVisibleFrom[sec] = section.occlusion.visibleFrom;
                }
            }
        }
        const bool loaded = ws::g_world.find(key) != ws::g_world.end();
        std::lock_guard<std::mutex> lock(g_drawMetadataMutex);
        if (loaded || metadata.passMask[0] || metadata.passMask[1]) g_drawMetadata[key] = metadata;
        else g_drawMetadata.erase(key);
    }

    void InvalidateDirtyOcclusionMetadataInternal() {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(g_drawMetadataMutex);
            for (const auto& dirty : ws::g_dirtyMask) {
                const auto found = g_drawMetadata.find(dirty.first);
                if (found == g_drawMetadata.end()) continue;
                const uint64_t oldMask = found->second.occlusionReadyMask;
                found->second.occlusionReadyMask &= ~dirty.second;
                changed = changed || oldMask != found->second.occlusionReadyMask;
            }
        }
        if (changed) g_occlusionMetadataEpoch.fetch_add(1, std::memory_order_relaxed);
    }

    void EraseDrawMetadata(uint64_t key) {
        std::lock_guard<std::mutex> lock(g_drawMetadataMutex);
        g_drawMetadata.erase(key);
    }

    std::vector<DrawChunkMetadata> SnapshotDrawMetadata() {
        std::vector<DrawChunkMetadata> snapshot;
        // Keep the metadata lock only while copying; all culling runs on the snapshot.
        std::lock_guard<std::mutex> lock(g_drawMetadataMutex);
        snapshot.reserve(g_drawMetadata.size());
        for (const auto& pair : g_drawMetadata) snapshot.push_back(pair.second);
        return snapshot;
    }

    std::shared_ptr<NativeCullFrame> BuildCullFrame(const NativeDrawView& view, uint64_t serial) {
        auto frame = std::make_shared<NativeCullFrame>();
        frame->serial = serial;
        frame->view = view;
        const double started = Plat_FloatTime();
        std::vector<DrawChunkMetadata> metadataSnapshot = SnapshotDrawMetadata();
        const double chunkW = g_promise.CS * g_promise.BS;
        const double chunkH = g_promise.CH * g_promise.BS;
        const double chunkHalfW = chunkW * 0.5, chunkHalfH = chunkH * 0.5;
        const double sectionSize = g_promise.SS * g_promise.BS;
        const double sectionHalf = sectionSize * 0.5;
        const OcclusionTraversal occlusion = BuildOcclusionTraversal(view, metadataSnapshot);
        for (int pass = 0; pass < 2; ++pass) {
            frame->pass[pass].occlusionCull = occlusion.requested;
            frame->pass[pass].occlusionApplied = occlusion.applied;
            frame->pass[pass].occlusionComponentSections = occlusion.componentSections;
            frame->pass[pass].occlusionReachableSections = occlusion.reachableSections;
        }

        frame->visible[0].reserve(metadataSnapshot.size());
        frame->visible[1].reserve(metadataSnapshot.size() / 4 + 1);
        if (view.collectKeys) frame->pass[0].visibleKeys.reserve(metadataSnapshot.size() * 12);
        for (const DrawChunkMetadata& metadata : metadataSnapshot) {
            if (metadata.hidden) continue;
            const double originX = metadata.cx * chunkW + view.offset[0];
            const double originY = metadata.cy * chunkW + view.offset[1];
            const double originZ = metadata.cz * chunkH + view.offset[2];
            const double centerX = originX + chunkHalfW, centerY = originY + chunkHalfW, centerZ = originZ + chunkHalfH;
            for (int pass = 0; pass < 2; ++pass) {
                uint64_t sectionMask = metadata.passMask[pass];
                if (!sectionMask) continue;
                NativeDrawResult& result = frame->pass[pass];
                ++result.testedChunks;
                result.testedSections += PopCount64(sectionMask);
                if (!AABBInDistance(view, centerX, centerY, centerZ, chunkHalfW, chunkHalfW, chunkHalfH)) {
                    ++result.distanceCulled; continue;
                }
                if (!AABBInFrustum(view, centerX, centerY, centerZ, chunkHalfW, chunkHalfW, chunkHalfH)) {
                    ++result.frustumCulled; continue;
                }
                uint64_t frustumVisibleMask = 0;
                uint64_t lodMask = 0;
                std::array<uint64_t, 6> directionalMasks{};
                for (int sec = 0; sec < g_promise.SPC; ++sec) {
                    if ((sectionMask & (1ull << sec)) == 0) continue;
                    const int sx = sec % g_promise.NSX;
                    const int sy = (sec / g_promise.NSX) % g_promise.NSX;
                    const int sz = sec / (g_promise.NSX * g_promise.NSX);
                    const double minX = originX + sx * sectionSize;
                    const double minY = originY + sy * sectionSize;
                    const double minZ = originZ + sz * sectionSize;
                    const double maxX = minX + sectionSize;
                    const double maxY = minY + sectionSize;
                    const double maxZ = minZ + sectionSize;
                    if (!AABBInFrustum(view, minX + sectionHalf, minY + sectionHalf, minZ + sectionHalf,
                        sectionHalf, sectionHalf, sectionHalf)) continue;
                    const uint64_t sectionBit = 1ull << sec;
                    frustumVisibleMask |= sectionBit;
                    if (pass == 0 && view.lod && (metadata.lodReadyMask & sectionBit) != 0
                        && DistanceSquaredToAABB(view, minX, minY, minZ, maxX, maxY, maxZ)
                            >= view.lodStartDistance * view.lodStartDistance)
                        lodMask |= sectionBit;
                    if (view.eye[2] >= minZ) directionalMasks[0] |= sectionBit; // +Z
                    if (view.eye[2] <= maxZ) directionalMasks[1] |= sectionBit; // -Z
                    if (view.eye[0] >= minX) directionalMasks[2] |= sectionBit; // +X
                    if (view.eye[0] <= maxX) directionalMasks[3] |= sectionBit; // -X
                    if (view.eye[1] >= minY) directionalMasks[4] |= sectionBit; // +Y
                    if (view.eye[1] <= maxY) directionalMasks[5] |= sectionBit; // -Y
                }
                if (!frustumVisibleMask) { ++result.frustumCulled; continue; }
                if (pass == 0 && view.collectKeys) {
                    AppendI32LE(result.visibleKeys, metadata.cx);
                    AppendI32LE(result.visibleKeys, metadata.cy);
                    AppendI32LE(result.visibleKeys, metadata.cz);
                }

                uint64_t visibleMask = frustumVisibleMask;
                if (occlusion.applied) {
                    uint64_t componentMask = 0;
                    const auto component = occlusion.component.find(metadata.key);
                    if (component != occlusion.component.end()) componentMask = component->second;
                    const uint64_t testedMask = visibleMask & componentMask;
                    result.occlusionTestedSections += PopCount64(testedMask);
                    if (testedMask) {
                        uint64_t reachableMask = 0;
                        const auto reachable = occlusion.reachable.find(metadata.key);
                        if (reachable != occlusion.reachable.end()) reachableMask = reachable->second;
                        const uint64_t culledMask = testedMask & ~reachableMask;
                        result.occlusionCulledSections += PopCount64(culledMask);
                        visibleMask &= ~culledMask;
                    }
                }
                if (!visibleMask) { ++result.occlusionCulledChunks; continue; }
                for (uint64_t& mask : directionalMasks) mask &= visibleMask;
                ++result.visibleChunks;
                result.visibleSections += PopCount64(visibleMask);
                VisibleDrawEntry entry;
                entry.key = metadata.key;
                entry.sectionMask = visibleMask;
                entry.lodMask = lodMask & visibleMask;
                entry.cx = metadata.cx; entry.cy = metadata.cy; entry.cz = metadata.cz;
                entry.directionalMasks = directionalMasks;
                frame->visible[pass].push_back(std::move(entry));
            }
        }
        const double elapsedUs = (Plat_FloatTime() - started) * 1e6;
        frame->pass[0].cullUs = elapsedUs;
        frame->pass[1].cullUs = elapsedUs;
        return frame;
    }

    void CullWorkerMain() {
        for (;;) {
            NativeDrawView view;
            uint64_t serial = 0;
            {
                std::unique_lock<std::mutex> lock(g_cullMutex);
                g_cullCv.wait(lock, [] { return g_cullStop || g_cullPending; });
                if (g_cullStop) return;
                view = g_cullRequest;
                view.collectKeys = g_cullCollectPending;
                g_cullCollectPending = false;
                serial = g_cullNextSerial;
                g_cullPending = false;
            }
            std::shared_ptr<const NativeCullFrame> completed;
            try { completed = BuildCullFrame(view, serial); }
            catch (...) {
                std::lock_guard<std::mutex> lock(g_cullMutex);
                g_cullFailed = true;
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(g_cullMutex);
                if (!completed->pass[0].visibleKeys.empty())
                    g_pendingVisibleKeys = completed->pass[0].visibleKeys;
                if (!g_latestCullFrame || completed->serial >= g_latestCullFrame->serial)
                    g_latestCullFrame = std::move(completed);
            }
        }
    }

    bool StartCullWorker() {
        std::lock_guard<std::mutex> lock(g_cullMutex);
        if (g_cullRunning) return true;
        g_cullStop = false;
        g_cullFailed = false;
        try { g_cullThread = std::thread(CullWorkerMain); }
        catch (...) { return false; }
        g_cullRunning = true;
        return true;
    }

    void StopCullWorker() {
        {
            std::lock_guard<std::mutex> lock(g_cullMutex);
            if (!g_cullRunning) {
                g_latestCullFrame.reset(); g_activeCullFrame.reset(); g_pendingVisibleKeys.clear();
                g_cullCollectPending = false; g_cullFailed = false;
                return;
            }
            g_cullStop = true;
            g_cullPending = false;
        }
        g_cullCv.notify_all();
        if (g_cullThread.joinable()) g_cullThread.join();
        std::lock_guard<std::mutex> lock(g_cullMutex);
        g_cullRunning = false;
        g_cullStop = false;
        g_cullCollectPending = false;
        g_cullFailed = false;
        g_latestCullFrame.reset();
        g_activeCullFrame.reset();
        g_pendingVisibleKeys.clear();
    }

    uint64_t SubmitCullRequest(const NativeDrawView& view) {
        uint64_t serial = 0;
        {
            std::lock_guard<std::mutex> lock(g_cullMutex);
            g_cullRequest = view;
            g_cullCollectPending = g_cullCollectPending || view.collectKeys;
            ++g_cullNextSerial;
            if (g_cullNextSerial == 0) ++g_cullNextSerial;
            serial = g_cullNextSerial;
            g_cullPending = true;
        }
        g_cullCv.notify_one();
        return serial;
    }

    std::shared_ptr<const NativeCullFrame> LatestCullFrame() {
        std::lock_guard<std::mutex> lock(g_cullMutex);
        return g_latestCullFrame;
    }

    void PublishCullFrame(const std::shared_ptr<const NativeCullFrame>& frame) {
        std::lock_guard<std::mutex> lock(g_cullMutex);
        if (!frame->pass[0].visibleKeys.empty()) g_pendingVisibleKeys = frame->pass[0].visibleKeys;
        if (!g_latestCullFrame || frame->serial >= g_latestCullFrame->serial) g_latestCullFrame = frame;
    }

    std::string TakePendingVisibleKeys() {
        std::lock_guard<std::mutex> lock(g_cullMutex);
        std::string keys;
        keys.swap(g_pendingVisibleKeys);
        return keys;
    }

    bool CullWorkerFailed() {
        std::lock_guard<std::mutex> lock(g_cullMutex);
        return g_cullFailed;
    }

    bool SameRenderFrame(const NativeDrawView& a, const NativeDrawView& b) {
        return a.offset[0] == b.offset[0] && a.offset[1] == b.offset[1] && a.offset[2] == b.offset[2]
            && a.lod == b.lod && a.lodStartChunks == b.lodStartChunks
            && a.occlusionCull == b.occlusionCull
            && (!a.occlusionCull || a.occlusionEpoch == b.occlusionEpoch);
    }

    bool DirtyBitSet(uint64_t key, int sec) {
        auto it = ws::g_dirtyMask.find(key);
        return it != ws::g_dirtyMask.end() && (it->second & (1ull << sec)) != 0;
    }

    bool GenerationMatches(uint64_t key, int sec, uint64_t generation) {
        auto it = g_generations.find(key);
        return it != g_generations.end() && it->second[sec] == generation;
    }

    int PushThinkResult(ILuaBase* LUA, size_t pending, int committed) {
        LUA->CreateTable();
        LUA->PushNumber((double)pending); LUA->SetField(-2, "pending");
        LUA->PushNumber((double)committed); LUA->SetField(-2, "committed");
        if (mcmesh::meshbuild::g_detailedProfile.enabled
            && mcmesh::meshbuild::g_detailedProfile.latest.sequence != 0) {
            const auto& p = mcmesh::meshbuild::g_detailedProfile.latest;
            LUA->CreateTable();
#define MC_PROFILE_NUMBER(name, value) LUA->PushNumber((double)(value)); LUA->SetField(-2, name)
            MC_PROFILE_NUMBER("sequence", p.sequence); MC_PROFILE_NUMBER("budgetMS", p.budgetMS);
            MC_PROFILE_NUMBER("totalUs", p.totalUs); MC_PROFILE_NUMBER("budgetOverrunUs", p.budgetOverrunUs);
            MC_PROFILE_NUMBER("workerStartUs", p.workerStartUs); MC_PROFILE_NUMBER("collectUs", p.collectUs);
            MC_PROFILE_NUMBER("finalizeUs", p.finalizeUs); MC_PROFILE_NUMBER("queueWaitUs", p.queueWaitUs);
            MC_PROFILE_NUMBER("stageUs", p.stageUs); MC_PROFILE_NUMBER("allocateUs", p.allocateUs);
            MC_PROFILE_NUMBER("uploadUs", p.uploadUs); MC_PROFILE_NUMBER("replaceUs", p.replaceUs);
            MC_PROFILE_NUMBER("destroyUs", p.destroyUs); MC_PROFILE_NUMBER("dirtyScanUs", p.dirtyScanUs);
            MC_PROFILE_NUMBER("captureUs", p.captureUs); MC_PROFILE_NUMBER("enqueueUs", p.enqueueUs);
            MC_PROFILE_NUMBER("resultsCollected", p.resultsCollected); MC_PROFILE_NUMBER("resultsVisited", p.resultsVisited);
            MC_PROFILE_NUMBER("committed", p.committed); MC_PROFILE_NUMBER("stale", p.stale); MC_PROFILE_NUMBER("failed", p.failed);
            MC_PROFILE_NUMBER("captureAttempts", p.captureAttempts); MC_PROFILE_NUMBER("captureSuccess", p.captureSuccess);
            MC_PROFILE_NUMBER("enqueueAttempts", p.enqueueAttempts); MC_PROFILE_NUMBER("enqueueSuccess", p.enqueueSuccess);
            MC_PROFILE_NUMBER("enqueueRejected", p.enqueueRejected); MC_PROFILE_NUMBER("meshesCreated", p.meshesCreated);
            MC_PROFILE_NUMBER("meshesDestroyed", p.meshesDestroyed); MC_PROFILE_NUMBER("batchesUploaded", p.batchesUploaded);
            MC_PROFILE_NUMBER("verticesUploaded", p.verticesUploaded); MC_PROFILE_NUMBER("bytesUploaded", p.bytesUploaded);
            MC_PROFILE_NUMBER("dirtyBefore", p.dirtyBefore);
            MC_PROFILE_NUMBER("dirtyAfter", p.dirtyAfter); MC_PROFILE_NUMBER("workerJobsBefore", p.workerJobsBefore);
            MC_PROFILE_NUMBER("workerJobsAfter", p.workerJobsAfter); MC_PROFILE_NUMBER("workerActiveBefore", p.workerActiveBefore);
            MC_PROFILE_NUMBER("workerActiveAfter", p.workerActiveAfter); MC_PROFILE_NUMBER("workerResultsBefore", p.workerResultsBefore);
            MC_PROFILE_NUMBER("workerResultsAfter", p.workerResultsAfter); MC_PROFILE_NUMBER("readyBefore", p.readyBefore);
            MC_PROFILE_NUMBER("readyAfter", p.readyAfter); MC_PROFILE_NUMBER("outstandingBefore", p.outstandingBefore);
            MC_PROFILE_NUMBER("outstandingAfter", p.outstandingAfter); MC_PROFILE_NUMBER("resultBytesBefore", p.resultBytesBefore);
            MC_PROFILE_NUMBER("resultBytesAfter", p.resultBytesAfter);
#undef MC_PROFILE_NUMBER
            LUA->SetField(-2, "profile");
        }
        if (mcmesh::meshbuild::g_faulted) {
            LUA->PushBool(true); LUA->SetField(-2, "failed");
            LUA->PushString(mcmesh::meshbuild::g_faultReason); LUA->SetField(-2, "fault");
        }
        return 1;
    }
}

namespace mcmesh::meshbuild {
    static void RecordTime(TimeProfile& profile, double us) {
        if (!std::isfinite(us) || us < 0) us = 0;
        ++profile.calls;
        profile.lastUs = us;
        profile.totalUs += us;
        if (us > profile.maxUs) profile.maxUs = us;
    }

    static void MergeTimeProfile(TimeProfile& destination, const TimeProfile& source) {
        if (source.calls == 0) return;
        destination.calls += source.calls;
        destination.lastUs = source.lastUs;
        destination.totalUs += source.totalUs;
        destination.maxUs = std::max(destination.maxUs, source.maxUs);
    }

    static size_t DirtySectionCount() {
        size_t dirty = 0;
        for (const auto& kv : ws::g_dirtyMask) dirty += (size_t)PopCount64(kv.second);
        return dirty;
    }

    static void ResetDetailedProfile() {
        const bool enabled = g_detailedProfile.enabled;
        const uint64_t generation = g_detailedProfile.generation + 1;
        g_detailedProfile = DetailedProfileState{};
        g_detailedProfile.enabled = enabled;
        g_detailedProfile.generation = generation == 0 ? 1 : generation;
        g_activeThinkProfile = nullptr;
    }

    static void RecordDetailedTime(TimeProfile& profile, double us) {
        if (g_detailedProfile.enabled) RecordTime(profile, us);
    }

    static void RecordOperation(const char* kind, double us, TimeProfile& profile) {
        if (!g_detailedProfile.enabled) return;
        RecordTime(profile, us);
        ++g_detailedProfile.operationSequence;
        g_detailedProfile.lastOperation = kind;
        g_detailedProfile.lastOperationUs = std::isfinite(us) && us >= 0 ? us : 0;
    }


    bool CaptureSectionSnapshot(uint64_t chunkKey, int sec, SectionSnapshot& out) {
        if (g_promise.SS <= 0 || sec < 0 || sec >= g_promise.SPC) return false;
        if (ws::g_world.find(chunkKey) == ws::g_world.end()) return false;

        const int cs = g_promise.CS;
        const int ch = g_promise.CH;
        const int ss = g_promise.SS;
        const int nsx = g_promise.NSX;
        const int sx = sec % nsx;
        const int sy = (sec / nsx) % nsx;
        const int sz = sec / (nsx * nsx);

        out.section = sec;
        out.coreSize = ss;
        out.halo = 2;
        out.side = ss + 4;
        out.lx0 = sx * ss;
        out.ly0 = sy * ss;
        out.lz0 = sz * ss;
        const size_t cellCount = size_t(out.side) * size_t(out.side) * size_t(out.side);
        out.cells.assign(cellCount, SnapshotCell{});

        int cx, cy, cz;
        ws::UnpackChunkKey(chunkKey, cx, cy, cz);
        out.cx=cx;out.cy=cy;out.cz=cz;

        lightatlas::SlotRef lightSlot;
        if (!lightatlas::EnsureChunkSlot(chunkKey, lightSlot)
            || !lightatlas::EnsureChunkTileUploaded(chunkKey, lightSlot.slot, lightSlot.generation))
            return false;
        out.lightSlot = lightSlot.slot;
        out.lightGeneration = lightSlot.generation;

        for (int z = 0; z < out.side; ++z)
        for (int y = 0; y < out.side; ++y)
        for (int x = 0; x < out.side; ++x) {
            int lx = out.lx0 + x - out.halo;
            int ly = out.ly0 + y - out.halo;
            int lz = out.lz0 + z - out.halo;
            int ncx = cx, ncy = cy, ncz = cz;
            if (lx < 0) { --ncx; lx += cs; } else if (lx >= cs) { ++ncx; lx -= cs; }
            if (ly < 0) { --ncy; ly += cs; } else if (ly >= cs) { ++ncy; ly -= cs; }
            if (lz < 0) { --ncz; lz += ch; } else if (lz >= ch) { ++ncz; lz -= ch; }

            auto wit = ws::g_world.find(ws::PackChunkKey(ncx, ncy, ncz));
            if (wit == ws::g_world.end()) continue;
            const int li = lx + ly * cs + lz * cs * cs;
            SnapshotCell& cell = out.cells[size_t(x) + size_t(y) * out.side
                + size_t(z) * out.side * out.side];
            cell.stateId = wit->second->stateId[li];
            const blockdefs::StateDef* stateDef = blockdefs::FindState(cell.stateId);
            if (cell.stateId != 0 && !stateDef) return false;
            if (stateDef) { cell.id = stateDef->blockId; cell.orient = stateDef->orient; }
        }
        return true;
    }

    static bool CellOccludesSection(const SnapshotCell& cell) {
        if (cell.stateId == 0 || cell.id == 0) return false;
        const blockdefs::BlockDef* definition = blockdefs::FindDef(cell.id);
        return definition && blockdefs::IsSolid(definition) && !blockdefs::IsTransparent(definition)
            && blockdefs::OrientFullCube(definition, cell.orient);
    }

    static SectionOcclusion BuildSectionOcclusion(const SectionSnapshot& snapshot) {
        SectionOcclusion result;
        const int size = snapshot.coreSize;
        if (size <= 0 || snapshot.side != size + snapshot.halo * 2) return result;
        const int plane = size * size;
        const int volume = plane * size;
        std::vector<uint8_t> open(size_t(volume), 0);
        int occluders = 0;
        for (int z = 0; z < size; ++z)
        for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const size_t snapshotIndex = size_t(x + snapshot.halo)
                + size_t(y + snapshot.halo) * snapshot.side
                + size_t(z + snapshot.halo) * snapshot.side * snapshot.side;
            const int localIndex = x + y * size + z * plane;
            const bool passable = !CellOccludesSection(snapshot.cells[snapshotIndex]);
            open[size_t(localIndex)] = passable ? 1 : 0;
            if (!passable) ++occluders;
        }

        result.ready = true;
        if (occluders < plane) {
            result.visibleFrom.fill(kAllSectionFaces);
            return result;
        }
        if (occluders == volume) return result;

        std::vector<uint8_t> visited(size_t(volume), 0);
        std::vector<int> queue;
        queue.reserve(size_t(volume));
        for (int start = 0; start < volume; ++start) {
            if (!open[size_t(start)] || visited[size_t(start)]) continue;
            const int startZ = start / plane;
            const int startRemainder = start - startZ * plane;
            const int startY = startRemainder / size;
            const int startX = startRemainder - startY * size;
            if (startX != 0 && startX != size - 1 && startY != 0 && startY != size - 1
                && startZ != 0 && startZ != size - 1) continue;

            queue.clear();
            queue.push_back(start);
            visited[size_t(start)] = 1;
            uint8_t touchedFaces = 0;
            for (size_t head = 0; head < queue.size(); ++head) {
                const int index = queue[head];
                const int z = index / plane;
                const int remainder = index - z * plane;
                const int y = remainder / size;
                const int x = remainder - y * size;
                if (z == size - 1) touchedFaces |= 1u << 0;
                if (z == 0) touchedFaces |= 1u << 1;
                if (x == size - 1) touchedFaces |= 1u << 2;
                if (x == 0) touchedFaces |= 1u << 3;
                if (y == size - 1) touchedFaces |= 1u << 4;
                if (y == 0) touchedFaces |= 1u << 5;

                for (int direction = 0; direction < 6; ++direction) {
                    const int nx = x + OCCLUSION_DX[direction];
                    const int ny = y + OCCLUSION_DY[direction];
                    const int nz = z + OCCLUSION_DZ[direction];
                    if (nx < 0 || nx >= size || ny < 0 || ny >= size || nz < 0 || nz >= size) continue;
                    const int neighbor = nx + ny * size + nz * plane;
                    if (!open[size_t(neighbor)] || visited[size_t(neighbor)]) continue;
                    visited[size_t(neighbor)] = 1;
                    queue.push_back(neighbor);
                }
            }
            for (int face = 0; face < 6; ++face)
                if (touchedFaces & (1u << face)) result.visibleFrom[face] |= touchedFaces;
        }
        return result;
    }

    bool BuildSectionVerts(const SectionSnapshot& snapshot, SectionBuild& out) {
        out = SectionBuild{};
        out.lightSlot = snapshot.lightSlot;
        out.lightGeneration = snapshot.lightGeneration;
        if (snapshot.coreSize <= 0 || snapshot.halo != 2 || snapshot.side != snapshot.coreSize + 4) { out.ok=false; return false; }
        const size_t side=size_t(snapshot.side);
        if (side > 64 || snapshot.cells.size()!=side*side*side) { out.ok=false; return false; }
        auto lock=blockdefs::LockForRead();
        if (!blockdefs::g_loaded || !blockdefs::g_atlas.valid) { out.ok=false; return false; }
        out.occlusion = BuildSectionOcclusion(snapshot);
        try { out.ok=emitters::Build(snapshot,out); } catch (...) { out.ok=false; }
        return out.ok;
    }

    enum class MeshCreateStatus { Empty, Created, Failed };
    struct MeshCreateResult {
        MeshCreateStatus status = MeshCreateStatus::Failed;
        IMesh* mesh = nullptr;
    };

    static MeshCreateResult CreateMeshFromBatch(IMatRenderContext* ctx, const VertexBatch& batch,
        IMaterial* mat, MeshDirectionalRanges& ranges, MeshStageStats* stats) {
        ranges = MeshDirectionalRanges{};
        if (batch.vertices == 0) return { MeshCreateStatus::Empty, nullptr };
        if (batch.vertices > mcmesh::MAX_VERTEX_COUNT || (batch.vertices % 3) != 0)
            return { MeshCreateStatus::Failed, nullptr };
        size_t verifiedVertices = 0;
        for (const auto& group : batch.groups) {
            if ((group.size() % 3) != 0) return { MeshCreateStatus::Failed, nullptr };
            verifiedVertices += group.size();
        }
        if (verifiedVertices != batch.vertices) return { MeshCreateStatus::Failed, nullptr };
        if (!SupportsNativeMeshVertexFormat(mat)) return { MeshCreateStatus::Failed, nullptr };
        if (!ctx) return { MeshCreateStatus::Failed, nullptr };

        const VertexFormat_t fmt = mat->GetVertexFormat();
        const double allocateT0 = stats ? Plat_FloatTime() : 0.0;
        IMesh* mesh = ctx->CreateStaticMesh(fmt & ~VERTEX_FORMAT_COMPRESSED,
            TEXTURE_GROUP_STATIC_VERTEX_BUFFER_OTHER, mat);
        if (stats) RecordTime(stats->allocate, (Plat_FloatTime() - allocateT0) * 1e6);
        if (!mesh) return { MeshCreateStatus::Failed, nullptr };

        const double uploadT0 = stats ? Plat_FloatTime() : 0.0;
        CMeshBuilder mb;
        mb.Begin(mesh, MATERIAL_TRIANGLES, (int)(batch.vertices / 3));
        int firstIndex = 0;
        for (int groupIndex = 0; groupIndex < kDirectionalGroupCount; ++groupIndex) {
            const auto& group = batch.groups[groupIndex];
            ranges.groups[groupIndex] = { firstIndex, (int)group.size() };
            for (const Vert& v : group) {
                mb.Position3f(v.pos[0], v.pos[1], v.pos[2]);
                mb.Normal3f(v.normal[0], v.normal[1], v.normal[2]);
                mb.TexCoord2f(0, v.u, v.v);
                mb.TexCoord2f(1, 0.0f, 0.0f);
                mb.Color4ub(v.rgba[0], v.rgba[1], v.rgba[2], v.rgba[3]);
                mb.AdvanceVertex();
            }
            firstIndex += (int)group.size();
        }
        mb.End();
        ranges.vertices = (int)batch.vertices;
        if (stats) {
            RecordTime(stats->upload, (Plat_FloatTime() - uploadT0) * 1e6);
            ++stats->meshesCreated;
            ++stats->batchesUploaded;
            stats->verticesUploaded += (uint64_t)batch.vertices;
            stats->bytesUploaded += (uint64_t)batch.vertices * sizeof(Vert);
        }
        return { MeshCreateStatus::Created, mesh };
    }

    static void DestroyMesh(IMatRenderContext* ctx, IMesh*& mesh, bool recordProfile) {
        if (!mesh) return;
        const double t0 = recordProfile && g_detailedProfile.enabled ? Plat_FloatTime() : 0.0;
        if (ctx) ctx->DestroyStaticMesh(mesh);
        if (recordProfile && g_detailedProfile.enabled) {
            const double us = (Plat_FloatTime() - t0) * 1e6;
            RecordTime(g_detailedProfile.destroy, us);
            ++g_detailedProfile.meshesDestroyed;
            if (g_activeThinkProfile) {
                g_activeThinkProfile->destroyUs += us;
                ++g_activeThinkProfile->meshesDestroyed;
            }
        }
        mesh = nullptr;
    }

    static bool CreateMeshList(IMatRenderContext* ctx, const PassVerts& pass, IMaterial* mat,
        std::vector<IMesh*>& out, std::vector<MeshDirectionalRanges>& outRanges, MeshStageStats* stats) {
        if (!ctx) return false;
        for (const auto& batch:pass.batches) {
            MeshDirectionalRanges ranges;
            MeshCreateResult made=CreateMeshFromBatch(ctx,batch,mat,ranges,stats);
            if (made.status==MeshCreateStatus::Failed) { for(IMesh*&m:out) DestroyMesh(ctx,m,false); out.clear(); outRanges.clear(); return false; }
            if (made.mesh) { out.push_back(made.mesh); outRanges.push_back(ranges); }
        }
        return out.size() == outRanges.size();
    }
    static void DestroyMeshList(IMatRenderContext* ctx,std::vector<IMesh*>& list,bool recordProfile=true){for(IMesh*&m:list)DestroyMesh(ctx,m,recordProfile);list.clear();}
    static void SubtractEmitterStats(EmitterStats& total,const EmitterStats& part){
        for(size_t i=0;i<6;++i){
            total.blocks[i]=part.blocks[i]>total.blocks[i]?0:total.blocks[i]-part.blocks[i];
            total.faces[i]=part.faces[i]>total.faces[i]?0:total.faces[i]-part.faces[i];
        }
    }
    bool StageSectionMeshes(const SectionBuild& build, SectionMeshes& staged, MeshStageStats* stats) {
        staged = SectionMeshes{};
        if (!g_matsys) return false;
        CMatRenderContextPtr ctx(g_matsys);
        if (!ctx) return false;
        try {
            if (!CreateMeshList(ctx, build.opaque, g_promise.MatOpaque, staged.opaque, staged.opaqueRanges, stats)
                || !CreateMeshList(ctx, build.translucent, g_promise.MatTranslucent, staged.translucent, staged.translucentRanges, stats)
                || (build.lodReady && !CreateMeshList(ctx, build.lodOpaque, g_promise.MatOpaque,
                    staged.lodOpaque, staged.lodOpaqueRanges, stats))) {
                DestroyMeshList(ctx, staged.opaque, false);
                DestroyMeshList(ctx, staged.translucent, false);
                DestroyMeshList(ctx, staged.lodOpaque, false);
                staged = SectionMeshes{};
                return false;
            }
            staged.opaqueVerts = (int)build.opaque.vertices;
            staged.translucentVerts = (int)build.translucent.vertices;
            staged.lodOpaqueVerts = (int)build.lodOpaque.vertices;
            staged.lodReady = build.lodReady;
            staged.lightSlot = build.lightSlot;
            staged.lightGeneration = build.lightGeneration;
            staged.emitters = build.emitters;
            staged.occlusion = build.occlusion;
            return true;
        }
        catch (...) {
            DestroyMeshList(ctx, staged.opaque, false);
            DestroyMeshList(ctx, staged.translucent, false);
            DestroyMeshList(ctx, staged.lodOpaque, false);
            staged = SectionMeshes{};
            return false;
        }
    }

    void DestroyStagedSectionMeshes(SectionMeshes& staged, bool recordProfile) {
        if (staged.opaque.empty() && staged.translucent.empty() && staged.lodOpaque.empty()) {
            staged = SectionMeshes{};
            return;
        }
        if (!g_matsys) return;
        CMatRenderContextPtr ctx(g_matsys);
        if (!ctx) return;
        DestroyMeshList(ctx, staged.opaque, recordProfile);
        DestroyMeshList(ctx, staged.translucent, recordProfile);
        DestroyMeshList(ctx, staged.lodOpaque, recordProfile);
        staged = SectionMeshes{};
    }

    bool CommitStagedSectionMeshes(uint64_t key, int sec, SectionMeshes& staged) {
        const double commitT0 = Plat_FloatTime();
        try {
            // Publishing a mesh is also the atlas synchronization barrier.
            // Never expose a staged mesh whose slot was reassigned or whose
            // complete tile has not been uploaded yet.
            if (!lightatlas::EnsureChunkTileUploaded(key, staged.lightSlot, staged.lightGeneration))
                return false;
            auto inserted = g_meshes.try_emplace(key);
            SectionMeshes& live = inserted.first->second.section[sec];
            SectionMeshes old = std::move(live);
            live = std::move(staged);
            SubtractEmitterStats(g_emitterStats, old.emitters);
            for (size_t i = 0; i < 6; ++i) {
                g_emitterStats.blocks[i] += live.emitters.blocks[i];
                g_emitterStats.faces[i] += live.emitters.faces[i];
            }
            RecordTime(g_meshCommitProfile, (Plat_FloatTime() - commitT0) * 1e6);
            if (g_detailedProfile.enabled) {
                const double us = (Plat_FloatTime() - commitT0) * 1e6;
                RecordTime(g_detailedProfile.replace, us);
                if (g_activeThinkProfile) g_activeThinkProfile->replaceUs += us;
            }
            const double destroyT0 = Plat_FloatTime();
            DestroyStagedSectionMeshes(old);
            RecordTime(g_meshDestroyProfile, (Plat_FloatTime() - destroyT0) * 1e6);
            RefreshDrawMetadata(key);
            return true;
        }
        catch (...) {
            return false;
        }
    }

    void DestroyChunkMeshes(uint64_t chunkKey) {
        auto it = g_meshes.find(chunkKey);
        if (it == g_meshes.end()) { EraseDrawMetadata(chunkKey); return; }
        if (!g_matsys) { EraseDrawMetadata(chunkKey); return; }
        CMatRenderContextPtr ctx(g_matsys);
        if (!ctx) { EraseDrawMetadata(chunkKey); return; }
        for (int i = 0; i < 64; ++i) {
            SubtractEmitterStats(g_emitterStats,it->second.section[i].emitters);
            DestroyMeshList(ctx, it->second.section[i].opaque);
            DestroyMeshList(ctx, it->second.section[i].translucent);
            DestroyMeshList(ctx, it->second.section[i].lodOpaque);
        }
        g_meshes.erase(it);
        EraseDrawMetadata(chunkKey);
    }

    void DestroyAllMeshes() {
        if (!g_matsys) {
            std::lock_guard<std::mutex> lock(g_drawMetadataMutex);
            g_drawMetadata.clear();
            return;
        }
        CMatRenderContextPtr ctx(g_matsys);
        if (!ctx) {
            std::lock_guard<std::mutex> lock(g_drawMetadataMutex);
            g_drawMetadata.clear();
            return;
        }
        for (auto& kv : g_meshes)
            for (int i = 0; i < 64; ++i) {
                SubtractEmitterStats(g_emitterStats,kv.second.section[i].emitters);
                DestroyMeshList(ctx, kv.second.section[i].opaque);
                DestroyMeshList(ctx, kv.second.section[i].translucent);
                DestroyMeshList(ctx, kv.second.section[i].lodOpaque);
            }
        g_meshes.clear();
        g_emitterStats={};
        std::lock_guard<std::mutex> lock(g_drawMetadataMutex);
        g_drawMetadata.clear();
    }

    void StopWorkers() {
        const double operationT0 = g_detailedProfile.enabled ? Plat_FloatTime() : 0.0;
        StopCullWorker();
        meshworker::Stop();
        for (auto& result : g_readyResults)
            DestroyStagedSectionMeshes(result.stagedMeshes);
        g_readyResults.clear();
        g_generations.clear();
        if (g_detailedProfile.enabled) RecordTime(g_detailedProfile.workerStop, (Plat_FloatTime() - operationT0) * 1e6);
    }

    int Think(ILuaBase* LUA) {
        if (!LUA->IsType(1, Type::Number)) { LUA->PushBool(false); return 1; }
        double budgetMS = LUA->GetNumber(1);
        if (!std::isfinite(budgetMS) || budgetMS < 0 || budgetMS > 100) { LUA->PushBool(false); return 1; }
        if (!Ready()) return PushThinkResult(LUA, 0, 0);
        if (g_faulted) return PushThinkResult(LUA, 0, 0);
        const double budgetSec = budgetMS / 1000.0;
        const double t0 = Plat_FloatTime();
        ThinkProfileSample sample;
        const bool profiling = g_detailedProfile.enabled;
        if (profiling) {
            sample.sequence = ++g_detailedProfile.nextThinkSequence;
            sample.budgetMS = budgetMS;
            sample.dirtyBefore = DirtySectionCount();
            const meshworker::Stats before = meshworker::GetStats();
            sample.workerJobsBefore = before.queuedJobs;
            sample.workerActiveBefore = before.activeJobs;
            sample.workerResultsBefore = before.queuedResults;
            sample.readyBefore = g_readyResults.size();
            sample.outstandingBefore = before.outstanding;
            sample.resultBytesBefore = before.resultBytes;
            g_activeThinkProfile = &sample;
        }

        const double workerStartT0 = profiling ? Plat_FloatTime() : 0.0;
        if (!meshworker::Start()) {
            meshworker::Stop();
            g_faulted = true;
            g_faultReason = "worker_start_failed";
            g_activeThinkProfile = nullptr;
            return PushThinkResult(LUA, 0, 0);
        }
        if (!StartCullWorker()) {
            meshworker::Stop();
            g_faulted = true;
            g_faultReason = "cull_worker_start_failed";
            g_activeThinkProfile = nullptr;
            return PushThinkResult(LUA, 0, 0);
        }
        if (profiling) {
            sample.workerStartUs = (Plat_FloatTime() - workerStartT0) * 1e6;
            RecordTime(g_detailedProfile.workerStart, sample.workerStartUs);
        }

        // Transfer ownership with a deque swap. No allocation can occur here, so
        // every outstanding/resultBytes reservation remains represented exactly once.
        const size_t readyBeforeCollect = g_readyResults.size();
        const double collectT0 = profiling ? Plat_FloatTime() : 0.0;
        meshworker::CollectResults(g_readyResults);
        if (profiling) {
            sample.collectUs = (Plat_FloatTime() - collectT0) * 1e6;
            sample.resultsCollected = g_readyResults.size() > readyBeforeCollect
                ? (uint64_t)(g_readyResults.size() - readyBeforeCollect) : 0;
            RecordTime(g_detailedProfile.collect, sample.collectUs);
        }

        int finalized = 0;
        const double finalizeT0 = profiling ? Plat_FloatTime() : 0.0;
        while (!g_readyResults.empty()) {
            if (finalized > 0 && (Plat_FloatTime() - t0) >= budgetSec) break;
            meshworker::Result& result = g_readyResults.front();
            if (profiling) {
                ++sample.resultsVisited;
                sample.queueWaitUs += result.queueWaitUs;
                RecordTime(g_detailedProfile.queueWait, result.queueWaitUs);
            }
            RecordTime(g_vertexBuildProfile, result.vertexBuildUs);
            if constexpr (meshworker::kWorkerCreatesMeshes) {
                RecordTime(g_meshStageProfile, result.meshStageUs);
                if (profiling) {
                    sample.stageUs += result.meshStageUs;
                    RecordTime(g_detailedProfile.stage, result.meshStageUs);
                    MergeTimeProfile(g_detailedProfile.allocate, result.stageStats.allocate);
                    MergeTimeProfile(g_detailedProfile.upload, result.stageStats.upload);
                    sample.allocateUs += result.stageStats.allocate.totalUs;
                    sample.uploadUs += result.stageStats.upload.totalUs;
                    sample.meshesCreated += result.stageStats.meshesCreated;
                    sample.batchesUploaded += result.stageStats.batchesUploaded;
                    sample.verticesUploaded += result.stageStats.verticesUploaded;
                    sample.bytesUploaded += result.stageStats.bytesUploaded;
                    g_detailedProfile.meshesCreated += result.stageStats.meshesCreated;
                    g_detailedProfile.batchesUploaded += result.stageStats.batchesUploaded;
                    g_detailedProfile.verticesUploaded += result.stageStats.verticesUploaded;
                    g_detailedProfile.bytesUploaded += result.stageStats.bytesUploaded;
                }
            }
            else if (result.ok) {
                const double stageT0 = Plat_FloatTime();
                result.meshesReady = StageSectionMeshes(result.build, result.stagedMeshes);
                result.meshStageUs = (Plat_FloatTime() - stageT0) * 1e6;
                RecordTime(g_meshStageProfile, result.meshStageUs);
                if (profiling) {
                    sample.stageUs += result.meshStageUs;
                    RecordTime(g_detailedProfile.stage, result.meshStageUs);
                }
                result.build = SectionBuild{};
                if (!result.meshesReady) {
                    result.ok = false;
                    result.meshCreateFailed = true;
                }
            }

            const bool chunkAlive = ws::g_world.find(result.chunkKey) != ws::g_world.end();
            const bool lightCurrent = chunkAlive
                && lightatlas::SlotMatches(result.chunkKey, result.lightSlot, result.lightGeneration)
                && result.stagedMeshes.lightSlot == result.lightSlot
                && result.stagedMeshes.lightGeneration == result.lightGeneration;
            // dirty bit 重新置位说明快照之后世界又变了；即使新 job 尚未投递也不能上屏。
            const bool current = chunkAlive
                && GenerationMatches(result.chunkKey, result.section, result.generation)
                && !DirtyBitSet(result.chunkKey, result.section)
                && lightCurrent;
            if (!current) {
                // jobsDropped 由 worker 统计队列丢弃；此处 stale 结果另行记账。
                DestroyStagedSectionMeshes(result.stagedMeshes);
                meshworker::RecordStaleDrop();
                meshworker::ReleaseResult(result.resultBytes);
                g_readyResults.pop_front();
                if (profiling) ++sample.stale;
                continue;
            }

            if (!result.ok || !result.meshesReady) {
                const bool meshCreateFailed = result.meshCreateFailed;
                DestroyStagedSectionMeshes(result.stagedMeshes);
                meshworker::ReleaseResult(result.resultBytes);
                g_readyResults.pop_front();
                g_faulted = true;
                if (meshCreateFailed) {
                    ++g_meshCreateFailures;
                    g_faultReason = "worker_mesh_create_failed";
                }
                else g_faultReason = "worker_build_failed";
                if (profiling) ++sample.failed;
                break;
            }
            if (!CommitStagedSectionMeshes(result.chunkKey, result.section, result.stagedMeshes)) {
                DestroyStagedSectionMeshes(result.stagedMeshes);
                meshworker::ReleaseResult(result.resultBytes);
                g_readyResults.pop_front();
                ++g_meshCreateFailures;
                g_faulted = true;
                g_faultReason = "mesh_create_failed";
                if (profiling) ++sample.failed;
                break;
            }
            g_lastBuildUs = result.buildUs;
            meshworker::ReleaseResult(result.resultBytes);
            g_readyResults.pop_front();
            ++finalized;
            if (profiling) ++sample.committed;
        }
        if (profiling) {
            sample.finalizeUs = (Plat_FloatTime() - finalizeT0) * 1e6;
            RecordTime(g_detailedProfile.finalize, sample.finalizeUs);
        }

        // 脏位是有界溢出缓冲。只有 job 成功入队后才清位；上限满则原位保留。
        const double dirtyScanT0 = profiling ? Plat_FloatTime() : 0.0;
        while (!g_faulted && !ws::g_dirtyQueue.empty()) {
            const uint64_t key = ws::g_dirtyQueue.front();
            auto mit = ws::g_dirtyMask.find(key);
            if (mit == ws::g_dirtyMask.end() || mit->second == 0) {
                ws::g_dirtyQueue.erase(ws::g_dirtyQueue.begin());
                if (mit != ws::g_dirtyMask.end()) ws::g_dirtyMask.erase(mit);
                continue;
            }
            if (ws::g_world.find(key) == ws::g_world.end()) {
                ws::g_dirtyQueue.erase(ws::g_dirtyQueue.begin());
                ws::g_dirtyMask.erase(mit);
                continue;
            }

            const int sec = LowestSetBit(mit->second);
            SectionSnapshot snapshot;
            const double captureT0 = profiling ? Plat_FloatTime() : 0.0;
            if (profiling) ++sample.captureAttempts;
            if (!CaptureSectionSnapshot(key, sec, snapshot)) {
                if (profiling) {
                    const double us = (Plat_FloatTime() - captureT0) * 1e6;
                    sample.captureUs += us;
                    RecordTime(g_detailedProfile.capture, us);
                }
                break;
            }
            if (profiling) {
                const double us = (Plat_FloatTime() - captureT0) * 1e6;
                sample.captureUs += us;
                ++sample.captureSuccess;
                RecordTime(g_detailedProfile.capture, us);
            }

            GenerationArray& generations = g_generations[key];
            uint64_t generation = ++g_nextGeneration;
            if (generation == 0) generation = ++g_nextGeneration;
            meshworker::Job job;
            job.chunkKey = key;
            job.section = sec;
            job.generation = generation;
            job.profile = profiling;
            job.snapshot = std::move(snapshot);
            const double enqueueT0 = profiling ? Plat_FloatTime() : 0.0;
            if (profiling) ++sample.enqueueAttempts;
            if (!meshworker::TryEnqueue(std::move(job))) {
                if (profiling) {
                    const double us = (Plat_FloatTime() - enqueueT0) * 1e6;
                    sample.enqueueUs += us;
                    ++sample.enqueueRejected;
                    RecordTime(g_detailedProfile.enqueue, us);
                }
                break;
            }
            if (profiling) {
                const double us = (Plat_FloatTime() - enqueueT0) * 1e6;
                sample.enqueueUs += us;
                ++sample.enqueueSuccess;
                RecordTime(g_detailedProfile.enqueue, us);
            }

            generations[sec] = generation;
            mit->second &= ~(1ull << sec);
            if (mit->second == 0) {
                ws::g_dirtyQueue.erase(ws::g_dirtyQueue.begin());
                ws::g_dirtyMask.erase(mit);
            }
        }

        g_lastThinkMS = (Plat_FloatTime() - t0) * 1000.0;
        int dirty = 0;
        for (const auto& kv : ws::g_dirtyMask) dirty += PopCount64(kv.second);
        const meshworker::Stats stats = meshworker::GetStats();
        const size_t pending = size_t(dirty) + stats.outstanding;
        if (profiling) {
            sample.dirtyScanUs = (Plat_FloatTime() - dirtyScanT0) * 1e6 - sample.captureUs - sample.enqueueUs;
            if (sample.dirtyScanUs < 0) sample.dirtyScanUs = 0;
            sample.totalUs = (Plat_FloatTime() - t0) * 1e6;
            sample.budgetOverrunUs = std::max(0.0, sample.totalUs - budgetMS * 1000.0);
            sample.dirtyAfter = (size_t)dirty;
            sample.workerJobsAfter = stats.queuedJobs;
            sample.workerActiveAfter = stats.activeJobs;
            sample.workerResultsAfter = stats.queuedResults;
            sample.readyAfter = g_readyResults.size();
            sample.outstandingAfter = stats.outstanding;
            sample.resultBytesAfter = stats.resultBytes;
            RecordTime(g_detailedProfile.think, sample.totalUs);
            RecordTime(g_detailedProfile.dirtyScan, sample.dirtyScanUs);
            g_detailedProfile.peakDirty = std::max(g_detailedProfile.peakDirty, std::max(sample.dirtyBefore, sample.dirtyAfter));
            g_detailedProfile.peakWorkerJobs = std::max(g_detailedProfile.peakWorkerJobs, std::max(sample.workerJobsBefore, sample.workerJobsAfter));
            g_detailedProfile.peakWorkerResults = std::max(g_detailedProfile.peakWorkerResults, std::max(sample.workerResultsBefore, sample.workerResultsAfter));
            g_detailedProfile.peakReadyResults = std::max(g_detailedProfile.peakReadyResults, std::max(sample.readyBefore, sample.readyAfter));
            g_detailedProfile.peakOutstanding = std::max(g_detailedProfile.peakOutstanding, std::max(sample.outstandingBefore, sample.outstandingAfter));
            g_detailedProfile.peakResultBytes = std::max(g_detailedProfile.peakResultBytes, std::max(sample.resultBytesBefore, sample.resultBytesAfter));
            g_detailedProfile.latest = sample;
            g_activeThinkProfile = nullptr;
        }
        return PushThinkResult(LUA, pending, finalized);
    }

    int DrawChunk(ILuaBase* LUA) {
        int cx, cy, cz;
        if (!ws::ReadChunkCoords(LUA, cx, cy, cz) || g_faulted) {
            LUA->PushBool(false);
            return 1;
        }
        if (!LUA->IsType(4, Type::Number)) { LUA->PushBool(false); return 1; }
        const double passD = LUA->GetNumber(4);
        if (!std::isfinite(passD) || passD != std::floor(passD) || (passD != 0 && passD != 1)) {
            LUA->PushBool(false);
            return 1;
        }
        const int pass = (int)passD;

        auto it = g_meshes.find(ws::PackChunkKey(cx, cy, cz));
        if (it != g_meshes.end())
            for (int i = 0; i < 64; ++i) {
                const auto& meshes = pass == 0 ? it->second.section[i].opaque : it->second.section[i].translucent;
                for (IMesh* mesh : meshes) if (mesh) mesh->Draw();
            }
        LUA->PushBool(true);
        return 1;
    }

    int DrawVisible(ILuaBase* LUA) {
        double passValue = 0;
        NativeDrawView view;
        double* numbers[] = {
            &passValue,
            &view.eye[0], &view.eye[1], &view.eye[2],
            &view.forward[0], &view.forward[1], &view.forward[2],
            &view.right[0], &view.right[1], &view.right[2],
            &view.up[0], &view.up[1], &view.up[2],
            &view.tanX, &view.tanY,
            &view.offset[0], &view.offset[1], &view.offset[2],
            &view.distance,
        };
        for (int i = 0; i < int(sizeof(numbers) / sizeof(numbers[0])); ++i)
            if (!ReadFiniteNumber(LUA, i + 1, *numbers[i])) { LUA->PushBool(false); return 1; }
        if ((passValue != 0 && passValue != 1) || view.tanX <= 0 || view.tanY <= 0 || view.distance < 0
            || !LUA->IsType(20, Type::Bool) || !LUA->IsType(21, Type::Bool) || !LUA->IsType(22, Type::Bool)
            || g_faulted || !g_matsys) { LUA->PushBool(false); return 1; }
        const int pass = int(passValue);
        view.frustum = LUA->GetBool(20);
        view.horizontalDistance = LUA->GetBool(21);
        view.collectKeys = LUA->GetBool(22);
        view.occlusionCull = g_occlusionCullEnabled.load(std::memory_order_relaxed);
        view.occlusionEpoch = g_occlusionMetadataEpoch.load(std::memory_order_relaxed);
        view.lod = g_lodDrawEnabled.load(std::memory_order_relaxed);
        view.lodStartChunks = g_lodStartChunks.load(std::memory_order_relaxed);
        view.lodStartDistance = view.lodStartChunks * g_promise.CS * g_promise.BS;

        if (!StartCullWorker() || CullWorkerFailed()) {
            g_faulted = true;
            g_faultReason = "cull_worker_failed";
            LUA->PushBool(false);
            return 1;
        }

        std::shared_ptr<const NativeCullFrame> frame = g_activeCullFrame;
        if (pass == 0 || !frame || !SameRenderFrame(frame->view, view)) {
            const uint64_t serial = SubmitCullRequest(view);
            frame = LatestCullFrame();
            // Startup and InfMap frame-offset jumps cannot consume an old coordinate frame.
            if (!frame || !SameRenderFrame(frame->view, view)) {
                try {
                    frame = BuildCullFrame(view, serial);
                    PublishCullFrame(frame);
                }
                catch (...) {
                    g_faulted = true;
                    g_faultReason = "cull_fallback_failed";
                    LUA->PushBool(false);
                    return 1;
                }
            }
            g_activeCullFrame = frame;
        }

        NativeDrawResult result;
        const NativeDrawResult& cullResult = frame->pass[pass];
        result.testedChunks = cullResult.testedChunks;
        result.distanceCulled = cullResult.distanceCulled;
        result.frustumCulled = cullResult.frustumCulled;
        result.testedSections = cullResult.testedSections;
        result.visibleChunks = cullResult.visibleChunks;
        result.visibleSections = cullResult.visibleSections;
        result.cullUs = cullResult.cullUs;
        result.directionalDraw = pass == 0 && g_directionalDrawEnabled.load(std::memory_order_relaxed);
        result.lod = pass == 0 && frame->view.lod;
        result.lodStartChunks = frame->view.lodStartChunks;
        result.occlusionCull = cullResult.occlusionCull;
        result.occlusionApplied = cullResult.occlusionApplied;
        result.occlusionTestedSections = cullResult.occlusionTestedSections;
        result.occlusionCulledSections = cullResult.occlusionCulledSections;
        result.occlusionCulledChunks = cullResult.occlusionCulledChunks;
        result.occlusionComponentSections = cullResult.occlusionComponentSections;
        result.occlusionReachableSections = cullResult.occlusionReachableSections;
        if (pass == 0) result.visibleKeys = TakePendingVisibleKeys();

        const double started = Plat_FloatTime();
        CMatRenderContextPtr ctx(g_matsys);
        if (!ctx) { LUA->PushBool(false); return 1; }
        ctx->MatrixMode(MATERIAL_MODEL);
        const double chunkW = g_promise.CS * g_promise.BS;
        const double chunkH = g_promise.CH * g_promise.BS;

        // Rendering stays on this thread, but it only consumes the worker-built visible list.
        for (const VisibleDrawEntry& visible : frame->visible[pass]) {
            if (g_hiddenChunks.find(visible.key) != g_hiddenChunks.end()) continue;
            auto chunk = g_meshes.find(visible.key);
            if (chunk == g_meshes.end()) continue;
            const double originX = visible.cx * chunkW + frame->view.offset[0];
            const double originY = visible.cy * chunkW + frame->view.offset[1];
            const double originZ = visible.cz * chunkH + frame->view.offset[2];
            bool pushed = false, chunkDrawn = false;
            uint64_t sectionMask = visible.sectionMask;
            while (sectionMask) {
                const int sec = LowestSetBit(sectionMask);
                sectionMask &= sectionMask - 1;
                if (sec < 0 || sec >= g_promise.SPC) continue;
                const SectionMeshes& section = chunk->second.section[sec];
                const uint64_t sectionBit = 1ull << sec;
                const bool useLod = pass == 0 && result.lod && (visible.lodMask & sectionBit) != 0
                    && section.lodReady && !section.lodOpaque.empty();
                const auto& meshes = useLod ? section.lodOpaque
                    : pass == 0 ? section.opaque : section.translucent;
                const auto& meshRanges = useLod ? section.lodOpaqueRanges
                    : pass == 0 ? section.opaqueRanges : section.translucentRanges;
                if (meshes.empty()) continue;
                if (!pushed) {
                    VMatrix model; model.Init(
                        1, 0, 0, float(originX),
                        0, 1, 0, float(originY),
                        0, 0, 1, float(originZ),
                        0, 0, 0, 1);
                    ctx->PushMatrix(); ctx->LoadMatrix(model); pushed = true;
                }
                const uint64_t meshesBefore = result.meshesDrawn;
                const uint64_t fullSectionVertices = pass == 0 ? (uint64_t)section.opaqueVerts : (uint64_t)section.translucentVerts;
                const uint64_t sectionVertices = useLod ? (uint64_t)section.lodOpaqueVerts : fullSectionVertices;
                result.verticesFullDetail += fullSectionVertices;
                if (useLod && fullSectionVertices > sectionVertices)
                    result.verticesLodCulled += fullSectionVertices - sectionVertices;
                result.verticesCandidate += sectionVertices;
                uint64_t selectedVertices = 0;
                if (result.directionalDraw && meshRanges.size() == meshes.size()) {
                    for (size_t meshIndex = 0; meshIndex < meshes.size(); ++meshIndex) {
                        IMesh* mesh = meshes[meshIndex];
                        if (!mesh) continue;
                        const MeshDirectionalRanges& ranges = meshRanges[meshIndex];
                        std::array<CPrimList, kDirectionalGroupCount> lists;
                        int listCount = 0;
                        for (int group = 0; group < 6; ++group) {
                            const DirectionalRange& range = ranges.groups[group];
                            if ((visible.directionalMasks[group] & sectionBit) == 0 || range.indexCount <= 0) continue;
                            lists[listCount++] = CPrimList(range.firstIndex, range.indexCount);
                            selectedVertices += (uint64_t)range.indexCount;
                        }
                        const DirectionalRange& unassigned = ranges.groups[kUnassignedDirectionalGroup];
                        if (unassigned.indexCount > 0) {
                            lists[listCount++] = CPrimList(unassigned.firstIndex, unassigned.indexCount);
                            selectedVertices += (uint64_t)unassigned.indexCount;
                        }
                        if (listCount > 0) {
                            mesh->Draw(lists.data(), listCount);
                            ++result.meshesDrawn;
                            if (useLod)++result.lodMeshesDrawn;
                        }
                    }
                }
                else {
                    for (IMesh* mesh : meshes) if (mesh) {
                        mesh->Draw();
                        ++result.meshesDrawn;
                        if (useLod)++result.lodMeshesDrawn;
                    }
                    selectedVertices = sectionVertices;
                }
                if (result.meshesDrawn != meshesBefore) {
                    ++result.drawnSections;
                    if (useLod) {
                        ++result.lodSectionsDrawn;
                        result.lodApplied = true;
                    }
                    result.verticesDrawn += selectedVertices;
                    chunkDrawn = true;
                }
            }
            if (pushed) ctx->PopMatrix();
            if (chunkDrawn) ++result.drawnChunks;
        }
        result.elapsedUs = (Plat_FloatTime() - started) * 1e6;
        result.verticesDirectionCulled = result.verticesCandidate > result.verticesDrawn
            ? result.verticesCandidate - result.verticesDrawn : 0;

        LUA->CreateTable();
#define MC_DRAW_RESULT(name, value) LUA->PushNumber(double(value)); LUA->SetField(-2, name)
        MC_DRAW_RESULT("tested", result.testedChunks); MC_DRAW_RESULT("drawn", result.drawnChunks);
        MC_DRAW_RESULT("distanceCulled", result.distanceCulled); MC_DRAW_RESULT("frustumCulled", result.frustumCulled);
        MC_DRAW_RESULT("visible", result.visibleChunks); MC_DRAW_RESULT("sectionsTested", result.testedSections);
        MC_DRAW_RESULT("sectionsVisible", result.visibleSections); MC_DRAW_RESULT("sectionsDrawn", result.drawnSections);
        MC_DRAW_RESULT("meshesDrawn", result.meshesDrawn); MC_DRAW_RESULT("nativeUs", result.elapsedUs);
        result.trianglesDrawn = result.verticesDrawn / 3;
        MC_DRAW_RESULT("verticesFullDetail", result.verticesFullDetail);
        MC_DRAW_RESULT("verticesCandidate", result.verticesCandidate); MC_DRAW_RESULT("verticesDrawn", result.verticesDrawn);
        MC_DRAW_RESULT("verticesLodCulled", result.verticesLodCulled);
        MC_DRAW_RESULT("verticesDirectionCulled", result.verticesDirectionCulled); MC_DRAW_RESULT("trianglesDrawn", result.trianglesDrawn);
        MC_DRAW_RESULT("lodSectionsDrawn", result.lodSectionsDrawn); MC_DRAW_RESULT("lodMeshesDrawn", result.lodMeshesDrawn);
        MC_DRAW_RESULT("lodStartChunks", result.lodStartChunks);
        MC_DRAW_RESULT("sectionsOcclusionTested", result.occlusionTestedSections);
        MC_DRAW_RESULT("sectionsOcclusionCulled", result.occlusionCulledSections);
        MC_DRAW_RESULT("chunksOcclusionCulled", result.occlusionCulledChunks);
        MC_DRAW_RESULT("occlusionComponentSections", result.occlusionComponentSections);
        MC_DRAW_RESULT("occlusionReachableSections", result.occlusionReachableSections);
        MC_DRAW_RESULT("cullUs", result.cullUs);
#undef MC_DRAW_RESULT
        LUA->PushBool(result.directionalDraw); LUA->SetField(-2, "directionalDraw");
        LUA->PushBool(result.lod); LUA->SetField(-2, "lod");
        LUA->PushBool(result.lodApplied); LUA->SetField(-2, "lodApplied");
        LUA->PushBool(result.occlusionCull); LUA->SetField(-2, "occlusionCull");
        LUA->PushBool(result.occlusionApplied); LUA->SetField(-2, "occlusionApplied");
        if (!result.visibleKeys.empty()) {
            LUA->PushString(result.visibleKeys.data(), (unsigned int)result.visibleKeys.size());
            LUA->SetField(-2, "visibleKeys");
        }
        return 1;
    }

    int SetChunkVisible(ILuaBase* LUA) {
        int cx, cy, cz;
        if (!ws::ReadChunkCoords(LUA, cx, cy, cz) || !LUA->IsType(4, Type::Bool)) {
            LUA->PushBool(false); return 1;
        }
        const uint64_t key = ws::PackChunkKey(cx, cy, cz);
        if (LUA->GetBool(4)) g_hiddenChunks.erase(key); else g_hiddenChunks.insert(key);
        RefreshDrawMetadata(key);
        LUA->PushBool(true); return 1;
    }

    int UnloadChunk(ILuaBase* LUA) {
        int cx, cy, cz;
        if (!ws::ReadChunkCoords(LUA, cx, cy, cz)) {
            LUA->PushBool(false);
            return 1;
        }
        const double operationT0 = g_detailedProfile.enabled ? Plat_FloatTime() : 0.0;
        const uint64_t key = ws::PackChunkKey(cx, cy, cz);
        g_hiddenChunks.erase(key);
        const size_t workerRemoved = meshworker::DiscardChunk(key);
        for (size_t i = 0; i < workerRemoved; ++i) meshworker::ReleaseResult();
        g_generations.erase(key);
        for (auto it = g_readyResults.begin(); it != g_readyResults.end();) {
            if (it->chunkKey == key) {
                DestroyStagedSectionMeshes(it->stagedMeshes);
                meshworker::ReleaseResult(it->resultBytes);
                meshworker::RecordStaleDrop();
                it = g_readyResults.erase(it);
            }
            else ++it;
        }
        DestroyChunkMeshes(key);
        const int returns = ws::UnloadChunk(LUA);
        lightatlas::ReleaseChunk(key);
        lightatlas::RefreshChangedChunks({ key });
        InvalidateDirtyOcclusionMetadataInternal();
        if (g_detailedProfile.enabled) RecordOperation("unload", (Plat_FloatTime() - operationT0) * 1e6, g_detailedProfile.unload);
        return returns;
    }

    void RefreshChunkDrawMetadata(uint64_t key) {
        RefreshDrawMetadata(key);
    }

    void InvalidateDirtyOcclusionMetadata() {
        InvalidateDirtyOcclusionMetadataInternal();
    }

    int ApplyChunk(ILuaBase* LUA) {
        int cx=0,cy=0,cz=0; unsigned int len=0;
        if (g_promise.SS<=0 || g_faulted || !ws::ReadChunkCoords(LUA,cx,cy,cz)) { LUA->PushBool(false); return 1; }
        const char* data=LUA->GetString(4,&len);
        if (!data || len!=kStateBlobBytes || !blockdefs::Seal()) { LUA->PushBool(false); return 1; }
        const int returns = ws::ApplyChunk(LUA);
        lightatlas::RefreshChangedChunks({ ws::PackChunkKey(cx, cy, cz) });
        RefreshDrawMetadata(ws::PackChunkKey(cx, cy, cz));
        InvalidateDirtyOcclusionMetadataInternal();
        return returns;
    }

    int CreateChunk(ILuaBase* LUA) {
        int cx=0,cy=0,cz=0;
        if (g_promise.SS<=0 || g_faulted || !blockdefs::Seal() || !ws::ReadChunkCoords(LUA,cx,cy,cz)) { LUA->PushBool(false); return 1; }
        const int returns = ws::CreateChunk(LUA);
        lightatlas::RefreshChangedChunks({ ws::PackChunkKey(cx, cy, cz) });
        RefreshDrawMetadata(ws::PackChunkKey(cx, cy, cz));
        InvalidateDirtyOcclusionMetadataInternal();
        return returns;
    }

    int ApplyChunkCells(ILuaBase* LUA) {
        int cx=0,cy=0,cz=0;
        if (g_promise.SS<=0 || g_faulted || !blockdefs::Seal() || !ws::ReadChunkCoords(LUA,cx,cy,cz)) { LUA->PushBool(false); return 1; }
        const int returns = ws::ApplyChunkCells(LUA);
        lightatlas::RefreshChangedChunks({ ws::PackChunkKey(cx, cy, cz) });
        RefreshDrawMetadata(ws::PackChunkKey(cx, cy, cz));
        InvalidateDirtyOcclusionMetadataInternal();
        return returns;
    }

    int SetCell(ILuaBase* LUA) {
        int cx=0,cy=0,cz=0;
        if (g_promise.SS<=0 || g_faulted || !blockdefs::Seal() || !ws::ReadChunkCoords(LUA,cx,cy,cz)) { LUA->PushBool(false); return 1; }
        const int returns = ws::SetCell(LUA);
        // worldstate::SetCell returns its exact incremental light diff through
        // PushChangedLightTable(), which synchronously refreshes every affected
        // atlas halo tile. Rebuilding the source neighbourhood again here did
        // the same GPU upload twice for every interactive edit. Before the
        // light field is ready it returns only the success bool; keep the
        // conservative full-neighbour refresh for that startup/disabled case.
        if (returns < 2)
            lightatlas::RefreshChangedChunks({ ws::PackChunkKey(cx, cy, cz) });
        RefreshDrawMetadata(ws::PackChunkKey(cx, cy, cz));
        InvalidateDirtyOcclusionMetadataInternal();
        return returns;
    }

    int GetCell(ILuaBase* LUA) {
        if (g_promise.SS<=0 || g_faulted || !blockdefs::Seal()) { LUA->PushNil(); return 1; }
        return ws::GetCell(LUA);
    }

    int GetChunkCells(ILuaBase* LUA) {
        if (g_promise.SS<=0 || g_faulted || !blockdefs::Seal()) { LUA->PushNil(); return 1; }
        return ws::GetChunkCells(LUA);
    }

    int RebuildBlockLight(ILuaBase* LUA) {
        if (g_promise.SS<=0 || g_faulted || !blockdefs::Seal()) { LUA->PushBool(false); return 1; }
        const int returns = ws::RebuildBlockLight(LUA);
        lightatlas::RefreshAllAssigned();
        return returns;
    }

    int GetBlockLightChunk(ILuaBase* LUA) {
        if (g_promise.SS<=0 || g_faulted || !blockdefs::Seal()) { LUA->PushNil(); return 1; }
        return ws::GetBlockLightChunk(LUA);
    }

    int GetSkyLightChunk(ILuaBase* LUA) {
        if (g_promise.SS<=0 || g_faulted || !blockdefs::Seal()) { LUA->PushNil(); return 1; }
        return ws::GetSkyLightChunk(LUA);
    }

    int GetChunkLightTile(ILuaBase* LUA) {
        if (g_promise.SS<=0 || g_faulted || !blockdefs::Seal()) { LUA->PushNil(); return 1; }
        return ws::GetChunkLightTile(LUA);
    }

    int StartBlockLightRebuild(ILuaBase* LUA) {
        if (g_promise.SS<=0 || g_faulted || !blockdefs::Seal()) { LUA->PushBool(false); return 1; }
        return ws::StartBlockLightRebuild(LUA);
    }

    int PollBlockLightRebuild(ILuaBase* LUA) {
        if (g_promise.SS<=0 || g_faulted) { LUA->PushBool(false); return 1; }
        return ws::PollBlockLightRebuild(LUA);
    }

    int CancelBlockLightRebuild(ILuaBase* LUA) {
        if (g_promise.SS<=0 || g_faulted) { LUA->PushBool(false); return 1; }
        return ws::CancelBlockLightRebuild(LUA);
    }

    int ClearWorld(ILuaBase* LUA) {
        patchdecode::CancelAll();
        const double operationT0 = g_detailedProfile.enabled ? Plat_FloatTime() : 0.0;
        StopCullWorker();
        const size_t workerRemoved = meshworker::DiscardAll();
        for (size_t i = 0; i < workerRemoved; ++i) meshworker::ReleaseResult();
        for (auto& result : g_readyResults) {
            DestroyStagedSectionMeshes(result.stagedMeshes);
            meshworker::ReleaseResult(result.resultBytes);
            meshworker::RecordStaleDrop();
        }
        g_readyResults.clear();
        g_generations.clear();
        g_hiddenChunks.clear();
        DestroyAllMeshes();
        blockdefs::Unseal();
        g_faulted = false; g_faultReason = ""; g_lastThinkMS=0; g_lastBuildUs=0; g_meshCreateFailures=0; g_emitterStats={}; g_resultBytes=0;
        g_vertexBuildProfile={}; g_meshStageProfile={}; g_meshCommitProfile={}; g_meshDestroyProfile={};
        const int returns = ws::ClearWorld(LUA);
        lightatlas::ClearWorld();
        if (g_detailedProfile.enabled) RecordOperation("clear", (Plat_FloatTime() - operationT0) * 1e6, g_detailedProfile.clear);
        return returns;
    }

    int Shutdown(ILuaBase* LUA) {
        patchdecode::Shutdown();
        const bool profiling = g_detailedProfile.enabled;
        const double operationT0 = profiling ? Plat_FloatTime() : 0.0;
        StopWorkers();
        ws::StopBlockLightWorker();
        DestroyAllMeshes();
        blockdefs::Clear();
        g_faulted = false; g_faultReason = ""; g_lastThinkMS=0; g_lastBuildUs=0; g_meshCreateFailures=0; g_emitterStats={}; g_resultBytes=0;
        g_vertexBuildProfile={}; g_meshStageProfile={}; g_meshCommitProfile={}; g_meshDestroyProfile={};
        ws::g_world.clear();
        ws::g_dirtyMask.clear();
        ws::g_dirtyQueue.clear(); g_hiddenChunks.clear(); ws::g_status={}; g_promise={};
        lightatlas::Shutdown();
        if (profiling) RecordOperation("shutdown", (Plat_FloatTime() - operationT0) * 1e6, g_detailedProfile.shutdown);
        g_detailedProfile.enabled = false;
        return 0;
    }

    int SetProfiling(ILuaBase* LUA) {
        if (!LUA->IsType(1, Type::Bool)) { LUA->PushBool(false); return 1; }
        const bool reset = LUA->Top() >= 2;
        if (reset && !LUA->IsType(2, Type::Bool)) { LUA->PushBool(false); return 1; }
        const bool enabled = LUA->GetBool(1);
        const bool shouldReset = reset && LUA->GetBool(2);
        if (shouldReset) ResetDetailedProfile();
        g_detailedProfile.enabled = enabled;
        g_activeThinkProfile = nullptr;
        LUA->PushBool(true);
        return 1;
    }

    int SetSimpleGreedy(ILuaBase* LUA) {
        if (!LUA->IsType(1, Type::Bool)) { LUA->PushBool(false); return 1; }
        const bool enabled = LUA->GetBool(1);
        if (enabled == g_simpleGreedyEnabled) { LUA->PushBool(true); return 1; }

        StopWorkers();
        g_simpleGreedyEnabled = enabled;
        for (const auto& chunk : ws::g_world) ws::MarkWholeChunkDirty(chunk.first);
        InvalidateDirtyOcclusionMetadataInternal();
        LUA->PushBool(true);
        return 1;
    }

    int SetDirectionalDraw(ILuaBase* LUA) {
        if (!LUA->IsType(1, Type::Bool)) { LUA->PushBool(false); return 1; }
        g_directionalDrawEnabled.store(LUA->GetBool(1), std::memory_order_relaxed);
        LUA->PushBool(true);
        return 1;
    }

    int SetOcclusionCull(ILuaBase* LUA) {
        if (!LUA->IsType(1, Type::Bool)) { LUA->PushBool(false); return 1; }
        g_occlusionCullEnabled.store(LUA->GetBool(1), std::memory_order_relaxed);
        LUA->PushBool(true);
        return 1;
    }

    int SetLod(ILuaBase* LUA) {
        double startChunks = 0;
        if (!LUA->IsType(1, Type::Bool) || !ReadFiniteNumber(LUA, 2, startChunks)
            || startChunks < 0 || startChunks > 4096) {
            LUA->PushBool(false);
            return 1;
        }
        const bool enabled = LUA->GetBool(1);
        if (enabled && !g_lodBuildEnabled) {
            StopWorkers();
            g_lodBuildEnabled = true;
            for (const auto& chunk : ws::g_world) ws::MarkWholeChunkDirty(chunk.first);
            InvalidateDirtyOcclusionMetadataInternal();
        }
        g_lodStartChunks.store(startChunks, std::memory_order_relaxed);
        g_lodDrawEnabled.store(enabled, std::memory_order_relaxed);
        LUA->PushBool(true);
        return 1;
    }

    int GetStats(ILuaBase* LUA) {
        int opaqueMeshCount = 0, translucentMeshCount = 0, lodOpaqueMeshCount = 0, lodReadySections = 0;
        long long opaqueVertCount = 0, translucentVertCount = 0, lodOpaqueVertCount = 0;
        for (const auto& kv : g_meshes)
            for (int i = 0; i < 64; ++i) {
                const SectionMeshes& sm = kv.second.section[i];
                if (!sm.opaque.empty()) { opaqueMeshCount += (int)sm.opaque.size(); opaqueVertCount += sm.opaqueVerts; }
                if (!sm.translucent.empty()) { translucentMeshCount += (int)sm.translucent.size(); translucentVertCount += sm.translucentVerts; }
                if (sm.lodReady && !sm.lodOpaque.empty()) ++lodReadySections;
                if (!sm.lodOpaque.empty()) { lodOpaqueMeshCount += (int)sm.lodOpaque.size(); lodOpaqueVertCount += sm.lodOpaqueVerts; }
            }
        const int meshCount = opaqueMeshCount + translucentMeshCount + lodOpaqueMeshCount;
        const long long vertCount = opaqueVertCount + translucentVertCount + lodOpaqueVertCount;

        int dirty = 0;
        for (const auto& kv : ws::g_dirtyMask) dirty += PopCount64(kv.second);
        const meshworker::Stats stats = meshworker::GetStats();
        const size_t pending = size_t(dirty) + stats.outstanding;
        size_t drawMetadataChunks = 0, occlusionReadySections = 0;
        {
            std::lock_guard<std::mutex> lock(g_drawMetadataMutex);
            drawMetadataChunks = g_drawMetadata.size();
            for (const auto& metadata : g_drawMetadata)
                occlusionReadySections += size_t(PopCount64(metadata.second.occlusionReadyMask));
        }
        bool cullWorkerRunning = false, cullPending = false, cullFailed = false;
        uint64_t cullSerial = 0, cullRequestSerial = 0;
        {
            std::lock_guard<std::mutex> lock(g_cullMutex);
            cullWorkerRunning = g_cullRunning;
            cullPending = g_cullPending;
            cullFailed = g_cullFailed;
            cullSerial = g_latestCullFrame ? g_latestCullFrame->serial : 0;
            cullRequestSerial = g_cullNextSerial;
        }

        LUA->CreateTable();
        LUA->PushNumber((double)ws::g_world.size()); LUA->SetField(-2, "mirrorChunks");
        LUA->PushNumber((double)meshCount); LUA->SetField(-2, "meshes");
        LUA->PushNumber((double)vertCount); LUA->SetField(-2, "vertices");
        LUA->PushBool(g_directionalDrawEnabled.load(std::memory_order_relaxed)); LUA->SetField(-2, "directionalDraw");
        LUA->PushBool(g_occlusionCullEnabled.load(std::memory_order_relaxed)); LUA->SetField(-2, "occlusionCull");
        LUA->PushBool(g_lodDrawEnabled.load(std::memory_order_relaxed)); LUA->SetField(-2, "lod");
        LUA->PushBool(g_lodBuildEnabled); LUA->SetField(-2, "lodBuildEnabled");
        LUA->PushNumber(g_lodStartChunks.load(std::memory_order_relaxed)); LUA->SetField(-2, "lodStartChunks");
        LUA->PushNumber(opaqueMeshCount); LUA->SetField(-2, "opaqueMeshes");
        LUA->PushNumber(translucentMeshCount); LUA->SetField(-2, "translucentMeshes");
        LUA->PushNumber(lodOpaqueMeshCount); LUA->SetField(-2, "lodOpaqueMeshes");
        LUA->PushNumber(lodReadySections); LUA->SetField(-2, "lodReadySections");
        LUA->PushNumber((double)opaqueVertCount); LUA->SetField(-2, "opaqueVertices");
        LUA->PushNumber((double)translucentVertCount); LUA->SetField(-2, "translucentVertices");
        LUA->PushNumber((double)lodOpaqueVertCount); LUA->SetField(-2, "lodOpaqueVertices");
        LUA->PushNumber((double)g_meshCreateFailures); LUA->SetField(-2, "meshCreateFailures");
        LUA->PushBool(g_faulted); LUA->SetField(-2, "faulted");
        LUA->PushString(g_faultReason); LUA->SetField(-2, "faultReason");
        LUA->PushNumber((double)pending); LUA->SetField(-2, "pendingSections");
        LUA->PushNumber(g_lastBuildUs); LUA->SetField(-2, "lastBuildUs");
        LUA->PushNumber(g_lastThinkMS); LUA->SetField(-2, "lastThinkMS");
        LUA->PushNumber(stats.workerCount); LUA->SetField(-2, "workerCount");
        LUA->PushNumber((double)stats.maxInFlight); LUA->SetField(-2, "maxInFlight");
        LUA->PushBool(stats.workerCreatesMeshes); LUA->SetField(-2, "workerCreatesMeshes");
        LUA->PushNumber((double)stats.queuedJobs); LUA->SetField(-2, "queuedJobs");
        LUA->PushNumber((double)stats.activeJobs); LUA->SetField(-2, "activeJobs");
        LUA->PushNumber((double)stats.outstanding); LUA->SetField(-2, "outstanding");
        LUA->PushNumber((double)stats.queuedResults); LUA->SetField(-2, "workerQueuedResults");
        LUA->PushNumber((double)g_readyResults.size()); LUA->SetField(-2, "readyResults");
        LUA->PushNumber((double)(stats.queuedResults + g_readyResults.size())); LUA->SetField(-2, "queuedResults");
        LUA->PushNumber((double)stats.jobsEnqueued); LUA->SetField(-2, "jobsEnqueued");
        LUA->PushNumber((double)stats.jobsDropped); LUA->SetField(-2, "jobsDropped");
        LUA->PushNumber((double)stats.resultBytes); LUA->SetField(-2, "resultBytes");
        LUA->PushBool(cullWorkerRunning); LUA->SetField(-2, "cullWorkerRunning");
        LUA->PushBool(cullPending); LUA->SetField(-2, "cullPending");
        LUA->PushBool(cullFailed); LUA->SetField(-2, "cullFailed");
        LUA->PushNumber((double)drawMetadataChunks); LUA->SetField(-2, "drawMetadataChunks");
        LUA->PushNumber((double)occlusionReadySections); LUA->SetField(-2, "occlusionReadySections");
        LUA->PushNumber((double)cullSerial); LUA->SetField(-2, "cullSerial");
        LUA->PushNumber((double)cullRequestSerial); LUA->SetField(-2, "cullRequestSerial");
        auto pushProfile = [LUA](const char* prefix, const TimeProfile& p) {
            const std::string base(prefix);
            LUA->PushNumber((double)p.calls); LUA->SetField(-2, (base + "Calls").c_str());
            LUA->PushNumber(p.lastUs); LUA->SetField(-2, (base + "LastUs").c_str());
            LUA->PushNumber(p.calls ? p.totalUs / (double)p.calls : 0.0); LUA->SetField(-2, (base + "AvgUs").c_str());
            LUA->PushNumber(p.maxUs); LUA->SetField(-2, (base + "MaxUs").c_str());
            LUA->PushNumber(p.totalUs / 1000.0); LUA->SetField(-2, (base + "TotalMS").c_str());
        };
        pushProfile("vertexBuild", g_vertexBuildProfile);
        pushProfile("meshStage", g_meshStageProfile);
        pushProfile("meshCommit", g_meshCommitProfile);
        pushProfile("meshDestroy", g_meshDestroyProfile);
        LUA->PushBool(true); LUA->SetField(-2, "profilingSupported");
        LUA->PushBool(g_detailedProfile.enabled); LUA->SetField(-2, "profilingEnabled");
        LUA->PushNumber((double)g_detailedProfile.generation); LUA->SetField(-2, "profilingGeneration");
        LUA->CreateTable();
        auto pushNestedProfile = [LUA](const char* name, const TimeProfile& p) {
            LUA->CreateTable();
            LUA->PushNumber((double)p.calls); LUA->SetField(-2, "calls");
            LUA->PushNumber(p.lastUs); LUA->SetField(-2, "lastUs");
            LUA->PushNumber(p.calls ? p.totalUs / (double)p.calls : 0.0); LUA->SetField(-2, "avgUs");
            LUA->PushNumber(p.maxUs); LUA->SetField(-2, "maxUs");
            LUA->PushNumber(p.totalUs / 1000.0); LUA->SetField(-2, "totalMS");
            LUA->SetField(-2, name);
        };
        LUA->PushBool(g_detailedProfile.enabled); LUA->SetField(-2, "enabled");
        LUA->PushNumber((double)g_detailedProfile.generation); LUA->SetField(-2, "generation");
        LUA->PushNumber((double)g_detailedProfile.operationSequence); LUA->SetField(-2, "operationSequence");
        LUA->PushString(g_detailedProfile.lastOperation); LUA->SetField(-2, "lastOperation");
        LUA->PushNumber(g_detailedProfile.lastOperationUs); LUA->SetField(-2, "lastOperationUs");
        LUA->PushNumber((double)g_detailedProfile.meshesCreated); LUA->SetField(-2, "meshesCreated");
        LUA->PushNumber((double)g_detailedProfile.meshesDestroyed); LUA->SetField(-2, "meshesDestroyed");
        LUA->PushNumber((double)g_detailedProfile.batchesUploaded); LUA->SetField(-2, "batchesUploaded");
        LUA->PushNumber((double)g_detailedProfile.verticesUploaded); LUA->SetField(-2, "verticesUploaded");
        LUA->PushNumber((double)g_detailedProfile.bytesUploaded); LUA->SetField(-2, "bytesUploaded");
        LUA->PushNumber((double)g_detailedProfile.peakDirty); LUA->SetField(-2, "peakDirty");
        LUA->PushNumber((double)g_detailedProfile.peakWorkerJobs); LUA->SetField(-2, "peakWorkerJobs");
        LUA->PushNumber((double)g_detailedProfile.peakWorkerResults); LUA->SetField(-2, "peakWorkerResults");
        LUA->PushNumber((double)g_detailedProfile.peakReadyResults); LUA->SetField(-2, "peakReadyResults");
        LUA->PushNumber((double)g_detailedProfile.peakOutstanding); LUA->SetField(-2, "peakOutstanding");
        LUA->PushNumber((double)g_detailedProfile.peakResultBytes); LUA->SetField(-2, "peakResultBytes");
        LUA->CreateTable();
        const ThinkProfileSample& latest = g_detailedProfile.latest;
        LUA->PushNumber((double)latest.sequence); LUA->SetField(-2, "sequence");
        LUA->PushNumber(latest.budgetMS); LUA->SetField(-2, "budgetMS");
        LUA->PushNumber(latest.totalUs); LUA->SetField(-2, "totalUs");
        LUA->PushNumber(latest.budgetOverrunUs); LUA->SetField(-2, "budgetOverrunUs");
        LUA->PushNumber(latest.stageUs); LUA->SetField(-2, "stageUs");
        LUA->PushNumber(latest.allocateUs); LUA->SetField(-2, "allocateUs");
        LUA->PushNumber(latest.uploadUs); LUA->SetField(-2, "uploadUs");
        LUA->PushNumber((double)latest.verticesUploaded); LUA->SetField(-2, "verticesUploaded");
        LUA->PushNumber((double)latest.bytesUploaded); LUA->SetField(-2, "bytesUploaded");
        LUA->PushNumber(latest.replaceUs); LUA->SetField(-2, "replaceUs");
        LUA->PushNumber(latest.destroyUs); LUA->SetField(-2, "destroyUs");
        LUA->PushNumber(latest.captureUs); LUA->SetField(-2, "captureUs");
        LUA->PushNumber(latest.enqueueUs); LUA->SetField(-2, "enqueueUs");
        LUA->PushNumber((double)latest.readyBefore); LUA->SetField(-2, "readyBefore");
        LUA->PushNumber((double)latest.readyAfter); LUA->SetField(-2, "readyAfter");
        LUA->PushNumber((double)latest.outstandingBefore); LUA->SetField(-2, "outstandingBefore");
        LUA->PushNumber((double)latest.outstandingAfter); LUA->SetField(-2, "outstandingAfter");
        LUA->SetField(-2, "latest");
        LUA->CreateTable();
        pushNestedProfile("think", g_detailedProfile.think); pushNestedProfile("workerStart", g_detailedProfile.workerStart);
        pushNestedProfile("collect", g_detailedProfile.collect); pushNestedProfile("finalize", g_detailedProfile.finalize);
        pushNestedProfile("queueWait", g_detailedProfile.queueWait); pushNestedProfile("stage", g_detailedProfile.stage);
        pushNestedProfile("allocate", g_detailedProfile.allocate); pushNestedProfile("upload", g_detailedProfile.upload);
        pushNestedProfile("replace", g_detailedProfile.replace); pushNestedProfile("destroy", g_detailedProfile.destroy);
        pushNestedProfile("dirtyScan", g_detailedProfile.dirtyScan); pushNestedProfile("capture", g_detailedProfile.capture);
        pushNestedProfile("enqueue", g_detailedProfile.enqueue); pushNestedProfile("unload", g_detailedProfile.unload);
        pushNestedProfile("clear", g_detailedProfile.clear); pushNestedProfile("shutdown", g_detailedProfile.shutdown);
        pushNestedProfile("workerStop", g_detailedProfile.workerStop);
        LUA->SetField(-2, "phases");
        LUA->SetField(-2, "profiling");
        LUA->PushNumber((double)ws::g_status.LastApplyDirty); LUA->SetField(-2, "lastApplyDirty");
        LUA->PushNumber((double)ws::g_status.BatchCalls); LUA->SetField(-2, "nativeBatchCalls");
        LUA->PushNumber((double)ws::g_status.BatchCells); LUA->SetField(-2, "nativeBatchCells");
        LUA->PushNumber(ws::g_status.BatchLastMS); LUA->SetField(-2, "nativeBatchLastMS");
        LUA->PushNumber(ws::g_status.BatchTotalMS); LUA->SetField(-2, "nativeBatchTotalMS");
        LUA->PushNumber((double)ws::g_status.SetCellCalls); LUA->SetField(-2, "nativeSetCellCalls");
        LUA->PushNumber(ws::g_status.SetCellLastUs); LUA->SetField(-2, "nativeSetCellLastUs");
        LUA->PushNumber(ws::g_status.SetCellTotalMS); LUA->SetField(-2, "nativeSetCellTotalMS");
        LUA->PushNumber((double)ws::g_status.LightRebuilds); LUA->SetField(-2, "nativeLightRebuilds");
        LUA->PushNumber(ws::g_status.LightLastMS); LUA->SetField(-2, "nativeLightLastMS");
        LUA->PushNumber((double)ws::g_status.LightSources); LUA->SetField(-2, "nativeLightSources");
        LUA->PushNumber((double)ws::g_status.LightProcessed); LUA->SetField(-2, "nativeLightProcessed");
        LUA->PushNumber((double)ws::g_status.LightWritten); LUA->SetField(-2, "nativeLightWritten");
        LUA->PushNumber(ws::g_status.SkyLastMS); LUA->SetField(-2, "nativeSkyLastMS");
        LUA->PushNumber((double)ws::g_status.SkySources); LUA->SetField(-2, "nativeSkySources");
        LUA->PushNumber((double)ws::g_status.SkyProcessed); LUA->SetField(-2, "nativeSkyProcessed");
        LUA->PushNumber((double)ws::g_status.SkyWritten); LUA->SetField(-2, "nativeSkyWritten");
        LUA->PushNumber((double)ws::g_status.IncrementalLightUpdates); LUA->SetField(-2, "nativeIncrementalLightUpdates");
        LUA->PushNumber((double)ws::g_status.IncrementalLightCells); LUA->SetField(-2, "nativeIncrementalLightCells");
        LUA->PushNumber(ws::g_status.IncrementalLightLastMS); LUA->SetField(-2, "nativeIncrementalLightLastMS");
        LUA->PushNumber((double)ws::g_status.AsyncPartialCommits); LUA->SetField(-2, "nativeAsyncPartialCommits");
        LUA->PushNumber((double)ws::g_status.AsyncDeferredChunks); LUA->SetField(-2, "nativeAsyncDeferredChunks");
        LUA->PushBool(ws::g_lightFieldReady); LUA->SetField(-2, "nativeLightFieldReady");
        LUA->PushNumber((double)ws::g_worldVersion); LUA->SetField(-2, "nativeWorldVersion");
        LUA->PushNumber((double)blockdefs::g_counts.states); LUA->SetField(-2, "stateDefinitions");
        LUA->PushBool(blockdefs::g_lightEmission.size()==blockdefs::g_states.size()
            && blockdefs::g_lightOpacity.size()==blockdefs::g_states.size()); LUA->SetField(-2, "lightDefinitionsLoaded");
        LUA->PushBool(blockdefs::g_visual.loaded); LUA->SetField(-2, "generatedCatalogLoaded");
        LUA->PushNumber((double)blockdefs::g_visual.catalogStateCount); LUA->SetField(-2, "generatedCatalogStates");
        LUA->PushNumber((double)blockdefs::g_visual.plans.size()); LUA->SetField(-2, "generatedPlans");
        LUA->PushNumber((double)blockdefs::g_visual.models.size()); LUA->SetField(-2, "generatedModels");
        LUA->PushNumber((double)blockdefs::g_visual.geometries.size()); LUA->SetField(-2, "generatedGeometries");
        LUA->PushNumber((double)blockdefs::g_visual.surfaceSets.size()); LUA->SetField(-2, "generatedSurfaceSets");
        LUA->PushNumber((double)blockdefs::g_visual.tintSlotCount); LUA->SetField(-2, "generatedTintSlots");
        static const char* emitterNames[6] = { "fullCube", "cross", "model", "shape", "connection", "liquid" };
        LUA->CreateTable();
        for (int i=0;i<6;++i) { LUA->CreateTable(); LUA->PushNumber((double)g_emitterStats.blocks[i]); LUA->SetField(-2,"blocks"); LUA->PushNumber((double)g_emitterStats.faces[i]); LUA->SetField(-2,"faces"); LUA->SetField(-2,emitterNames[i]); }
        LUA->SetField(-2,"emitters");
        return 1;
    }

    int DebugBuildSection(ILuaBase* LUA) {
        int cx, cy, cz;
        if (!ws::ReadChunkCoords(LUA, cx, cy, cz)) return 0;
        if (!LUA->IsType(4, Type::Number)) return 0;
        const double secD = LUA->GetNumber(4);
        if (!std::isfinite(secD) || secD != std::floor(secD) || secD < 0 || secD >= g_promise.SPC) return 0;
        if (!Ready()) return 0;

        SectionSnapshot snapshot;
        if (!CaptureSectionSnapshot(ws::PackChunkKey(cx, cy, cz), (int)secD, snapshot)) return 0;
        SectionBuild scratch;
        const double t0 = Plat_FloatTime();
        if (!BuildSectionVerts(snapshot, scratch)) return 0;
        const double us = (Plat_FloatTime() - t0) * 1e6;
        const int faces = scratch.opaque.faces + scratch.translucent.faces;
        const size_t verts = scratch.opaque.vertices + scratch.translucent.vertices;

        LUA->CreateTable();
        LUA->PushNumber(faces); LUA->SetField(-2, "faces");
        LUA->PushNumber((double)verts); LUA->SetField(-2, "verts");
        LUA->PushNumber(scratch.opaque.faces); LUA->SetField(-2, "opaqueFaces");
        LUA->PushNumber((double)scratch.opaque.vertices); LUA->SetField(-2, "opaqueVerts");
        LUA->PushNumber(scratch.translucent.faces); LUA->SetField(-2, "translucentFaces");
        LUA->PushNumber((double)scratch.translucent.vertices); LUA->SetField(-2, "translucentVerts");
        LUA->PushBool(scratch.lodReady); LUA->SetField(-2, "lodReady");
        LUA->PushNumber(scratch.lodOpaque.faces); LUA->SetField(-2, "lodOpaqueFaces");
        LUA->PushNumber((double)scratch.lodOpaque.vertices); LUA->SetField(-2, "lodOpaqueVerts");
        LUA->PushNumber(us); LUA->SetField(-2, "buildUs");
        static const char* emitterNames[6] = { "fullCube", "cross", "model", "shape", "connection", "liquid" };
        LUA->CreateTable();
        for (int i=0; i<6; ++i) {
            LUA->PushNumber((double)scratch.emitters.faces[i]);
            LUA->SetField(-2, emitterNames[i]);
        }
        LUA->SetField(-2, "emitterFaces");
        return 1;
    }
}
