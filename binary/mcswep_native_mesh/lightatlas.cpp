#include "pch.h"
#include "lightatlas.h"

#include "global.h"
#include "worldstate.h"

#include "public/materialsystem/imaterialvar.h"
#include "public/materialsystem/itexture.h"
#include "public/pixelwriter.h"
#include "public/vtf/vtf.h"
#include "public/tier0/basetypes.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

using namespace GarrysMod::Lua;

namespace mcmesh::lightatlas {
    namespace {
        constexpr const char* kTextureName = "mc_gpu_chunk_light_atlas_native1";
        constexpr uint8_t kFullbrightPacked = 0xff;

        struct SlotState {
            uint64_t key = 0;
            uint64_t generation = 0;
            bool occupied = false;
        };

        struct ChunkSlot {
            uint16_t slot = 0;
            uint64_t generation = 0;
            bool dirty = true;
            bool uploaded = false;
            bool fallback = true;
        };

        struct AtlasStats {
            uint64_t assignments = 0;
            uint64_t fallbackAssignments = 0;
            uint64_t releases = 0;
            uint64_t tileBuilds = 0;
            uint64_t tileUploads = 0;
            uint64_t uploadRects = 0;
            uint64_t generationRejects = 0;
            uint64_t capacityRejects = 0;
        };

        std::vector<uint8_t> g_backing;
        std::array<SlotState, kSlotCount> g_slots{};
        std::unordered_map<uint64_t, ChunkSlot> g_chunkSlots;
        std::vector<uint16_t> g_freeSlots;
        uint64_t g_nextGeneration = 0;
        ITexture* g_texture = nullptr;
        bool g_initialized = false;
        bool g_textureReady = false;
        AtlasStats g_stats{};

        class BackingRegenerator final : public ITextureRegenerator {
        public:
            void RegenerateTextureBits(ITexture*, IVTFTexture* vtf, Rect_t* requested) override {
                if (!vtf || g_backing.size() != size_t(kAtlasSize) * kAtlasSize * 4) return;
                Rect_t rect{};
                rect.x = 0;
                rect.y = 0;
                rect.width = kAtlasSize;
                rect.height = kAtlasSize;
                if (requested) rect = *requested;
                rect.x = std::clamp(rect.x, 0, kAtlasSize);
                rect.y = std::clamp(rect.y, 0, kAtlasSize);
                rect.width = std::clamp(rect.width, 0, kAtlasSize - rect.x);
                rect.height = std::clamp(rect.height, 0, kAtlasSize - rect.y);
                if (rect.width <= 0 || rect.height <= 0) return;

                uint8_t* destination = vtf->ImageData(0, 0, 0);
                if (!destination) return;
                const int destinationStride = vtf->RowSizeInBytes(0);
                const ImageFormat format = vtf->Format();

                // Source creates a full-sized scratch VTF even for Download(Rect_t*).
                // Its actual format is selected from the texture flags and is commonly
                // BGRA8888 on D3D, so copying our logical RGBA bytes verbatim corrupts
                // both channel order and (without EIGHTBITALPHA) row stride.
                if (format == IMAGE_FORMAT_RGBA8888) {
                    for (int row = 0; row < rect.height; ++row) {
                        const uint8_t* source = g_backing.data()
                            + (size_t(rect.y + row) * kAtlasSize + size_t(rect.x)) * 4;
                        uint8_t* target = destination + size_t(rect.y + row) * destinationStride
                            + size_t(rect.x) * 4;
                        std::memcpy(target, source, size_t(rect.width) * 4);
                    }
                    return;
                }

                if (format == IMAGE_FORMAT_BGRA8888) {
                    for (int row = 0; row < rect.height; ++row) {
                        const uint8_t* source = g_backing.data()
                            + (size_t(rect.y + row) * kAtlasSize + size_t(rect.x)) * 4;
                        uint8_t* target = destination + size_t(rect.y + row) * destinationStride
                            + size_t(rect.x) * 4;
                        for (int col = 0; col < rect.width; ++col) {
                            target[0] = source[2];
                            target[1] = source[1];
                            target[2] = source[0];
                            target[3] = source[3];
                            source += 4;
                            target += 4;
                        }
                    }
                    return;
                }

                CPixelWriter writer;
                writer.SetPixelMemory(format, destination, destinationStride);
                for (int row = 0; row < rect.height; ++row) {
                    const uint8_t* source = g_backing.data()
                        + (size_t(rect.y + row) * kAtlasSize + size_t(rect.x)) * 4;
                    writer.Seek(rect.x, rect.y + row);
                    for (int col = 0; col < rect.width; ++col) {
                        writer.WritePixel(source[0], source[1], source[2], source[3]);
                        source += 4;
                    }
                }
            }

            void Release() override {}
        };

        BackingRegenerator g_regenerator;

        uint64_t NextGeneration() {
            uint64_t generation = ++g_nextGeneration;
            if (generation == 0) generation = ++g_nextGeneration;
            return generation;
        }

        void ResetAllocator() {
            g_slots = {};
            g_chunkSlots.clear();
            g_freeSlots.clear();
            g_freeSlots.reserve(kRealCapacity);
            for (int slot = kSlotCount - 1; slot >= 1; --slot)
                g_freeSlots.push_back(uint16_t(slot));
        }

        void SlotOrigin(uint16_t slot, int& x, int& y) {
            const int row = int(slot) / kTilesPerRow;
            const int col = int(slot) - row * kTilesPerRow;
            x = col * kTileWidth;
            y = row * kTileHeight;
        }

        const worldstate::ChunkMirror* FindChunk(int cx, int cy, int cz) {
            const auto found = worldstate::g_world.find(worldstate::PackChunkKey(cx, cy, cz));
            return found == worldstate::g_world.end() ? nullptr : found->second.get();
        }

        uint8_t SamplePacked(const worldstate::ChunkMirror* neighbors[3][3][3], int lx, int ly, int lz) {
            const int cs = g_promise.CS;
            const int ch = g_promise.CH;
            const int nx = lx < 0 ? 0 : (lx >= cs ? 2 : 1);
            const int ny = ly < 0 ? 0 : (ly >= cs ? 2 : 1);
            const int nz = lz < 0 ? 0 : (lz >= ch ? 2 : 1);
            const int localX = lx < 0 ? cs - 1 : (lx >= cs ? 0 : lx);
            const int localY = ly < 0 ? cs - 1 : (ly >= cs ? 0 : ly);
            const int localZ = lz < 0 ? ch - 1 : (lz >= ch ? 0 : lz);
            const int clampedZ = std::clamp(lz, 0, ch - 1);

            const worldstate::ChunkMirror* source = neighbors[nx][ny][nz];
            int sourceZ = localZ;
            if (!source) {
                source = neighbors[nx][ny][1];
                sourceZ = clampedZ;
            }
            if (!source || !source->lightValid) return kFullbrightPacked;
            const int li = localX + localY * cs + sourceZ * cs * cs;
            return uint8_t((source->blockLight[li] & 15) | ((source->skyLight[li] & 15) << 4));
        }

        bool BuildTile(uint64_t chunkKey, ChunkSlot& owner) {
            if (owner.slot == 0) {
                owner.dirty = false;
                owner.uploaded = true;
                return true;
            }
            int cx = 0, cy = 0, cz = 0;
            worldstate::UnpackChunkKey(chunkKey, cx, cy, cz);
            const worldstate::ChunkMirror* center = FindChunk(cx, cy, cz);
            if (!center) return false;

            int originX = 0, originY = 0;
            SlotOrigin(owner.slot, originX, originY);
            if (!center->lightValid) {
                for (int row = 0; row < kTileHeight; ++row) {
                    uint8_t* target = g_backing.data()
                        + (size_t(originY + row) * kAtlasSize + size_t(originX)) * 4;
                    std::memset(target, kFullbrightPacked, size_t(kTileWidth) * 4);
                }
            }
            else {
                const worldstate::ChunkMirror* neighbors[3][3][3]{};
                for (int dz = -1; dz <= 1; ++dz)
                for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    neighbors[dx + 1][dy + 1][dz + 1] = FindChunk(cx + dx, cy + dy, cz + dz);

                for (int ly = -1; ly <= g_promise.CS; ++ly)
                for (int lx = -1; lx <= g_promise.CS; ++lx) {
                    const int pixelX = (lx + 1) + (ly + 1) * (g_promise.CS + 2);
                    for (int pixelY = 0; pixelY < kTileHeight; ++pixelY) {
                        uint8_t* pixel = g_backing.data()
                            + (size_t(originY + pixelY) * kAtlasSize + size_t(originX + pixelX)) * 4;
                        for (int channel = 0; channel < 4; ++channel) {
                            const int lz = pixelY * 4 + channel - 1;
                            pixel[channel] = lz <= g_promise.CH
                                ? SamplePacked(neighbors, lx, ly, lz)
                                : kFullbrightPacked;
                        }
                    }
                }
            }
            ++g_stats.tileBuilds;
            return true;
        }

        bool UploadSlots(std::vector<uint16_t> slots) {
            if (slots.empty()) return true;
            if (!Initialize()) return false;
            std::sort(slots.begin(), slots.end());
            slots.erase(std::unique(slots.begin(), slots.end()), slots.end());

            size_t index = 0;
            while (index < slots.size()) {
                const int row = int(slots[index]) / kTilesPerRow;
                int firstCol = int(slots[index]) % kTilesPerRow;
                int lastCol = firstCol;
                ++index;
                while (index < slots.size()) {
                    const int nextRow = int(slots[index]) / kTilesPerRow;
                    const int nextCol = int(slots[index]) % kTilesPerRow;
                    if (nextRow != row || nextCol != lastCol + 1) break;
                    lastCol = nextCol;
                    ++index;
                }
                Rect_t rect{};
                rect.x = firstCol * kTileWidth;
                rect.y = row * kTileHeight;
                rect.width = (lastCol - firstCol + 1) * kTileWidth;
                rect.height = kTileHeight;
                g_texture->Download(&rect);
                ++g_stats.uploadRects;
                g_stats.tileUploads += uint64_t(lastCol - firstCol + 1);
            }
            return true;
        }

        bool BuildAndUploadKeys(const std::unordered_set<uint64_t>& keys) {
            std::vector<uint16_t> uploadSlots;
            uploadSlots.reserve(keys.size());
            std::vector<uint64_t> builtKeys;
            builtKeys.reserve(keys.size());
            for (uint64_t key : keys) {
                auto found = g_chunkSlots.find(key);
                if (found == g_chunkSlots.end()) continue;
                ChunkSlot& owner = found->second;
                if (owner.slot == 0) {
                    owner.dirty = false;
                    owner.uploaded = true;
                    continue;
                }
                if (!owner.dirty && owner.uploaded) continue;
                // Readiness is committed only after the complete GPU Download
                // succeeds. A failed build/upload remains dirty and cannot be
                // mistaken for the previous contents of this stable slot.
                owner.dirty = true;
                owner.uploaded = false;
                if (!BuildTile(key, owner)) return false;
                uploadSlots.push_back(owner.slot);
                builtKeys.push_back(key);
            }
            if (!UploadSlots(std::move(uploadSlots))) return false;
            for (uint64_t key : builtKeys) {
                auto found = g_chunkSlots.find(key);
                if (found != g_chunkSlots.end()) {
                    found->second.dirty = false;
                    found->second.uploaded = true;
                }
            }
            return true;
        }

        void CollectAllocatedNeighborhood(uint64_t changedKey, std::unordered_set<uint64_t>& out) {
            int cx = 0, cy = 0, cz = 0;
            worldstate::UnpackChunkKey(changedKey, cx, cy, cz);
            for (int dz = -1; dz <= 1; ++dz)
            for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const uint64_t key = worldstate::PackChunkKey(cx + dx, cy + dy, cz + dz);
                auto found = g_chunkSlots.find(key);
                if (found == g_chunkSlots.end()) continue;
                found->second.dirty = true;
                out.insert(key);
            }
        }
    }

    bool Initialize() {
        if (g_textureReady && g_texture) return true;
        if (!g_matsys) return false;
        if (!g_initialized) {
            try {
                g_backing.assign(size_t(kAtlasSize) * kAtlasSize * 4, kFullbrightPacked);
                ResetAllocator();
            }
            catch (...) {
                g_backing.clear();
                return false;
            }
            g_initialized = true;
        }
        if (!g_texture) {
            const int flags = TEXTUREFLAGS_POINTSAMPLE | TEXTUREFLAGS_CLAMPS | TEXTUREFLAGS_CLAMPT
                | TEXTUREFLAGS_NOMIP | TEXTUREFLAGS_NOLOD | TEXTUREFLAGS_PROCEDURAL
                | TEXTUREFLAGS_EIGHTBITALPHA | TEXTUREFLAGS_SINGLECOPY;
            g_texture = g_matsys->CreateProceduralTexture(kTextureName, TEXTURE_GROUP_OTHER,
                kAtlasSize, kAtlasSize, IMAGE_FORMAT_RGBA8888, flags);
            if (!g_texture || g_texture->IsError()) {
                g_texture = nullptr;
                return false;
            }
            g_texture->SetTextureRegenerator(&g_regenerator);
            g_texture->Download();
        }
        g_textureReady = true;
        return true;
    }

    bool BindMaterial(const char* materialName) {
        if (!materialName || !*materialName || !Initialize()) return false;
        IMaterial* material = g_matsys->FindMaterial(materialName, TEXTURE_GROUP_OTHER, false);
        if (!material || material->IsErrorMaterial()) return false;
        bool found = false;
        IMaterialVar* textureVar = material->FindVar("$texture1", &found, false);
        if (!found || !textureVar) return false;
        textureVar->SetTextureValue(g_texture);
        return true;
    }

    bool EnsureChunkSlot(uint64_t chunkKey, SlotRef& out) {
        auto existing = g_chunkSlots.find(chunkKey);
        if (existing != g_chunkSlots.end()) {
            out = { existing->second.slot, existing->second.generation, existing->second.fallback };
            return true;
        }
        if (worldstate::g_world.find(chunkKey) == worldstate::g_world.end()) return false;

        ChunkSlot owner;
        owner.generation = NextGeneration();
        if (g_freeSlots.empty()) {
            owner.slot = 0;
            owner.fallback = true;
            owner.dirty = false;
            owner.uploaded = true;
            ++g_stats.fallbackAssignments;
            ++g_stats.capacityRejects;
        }
        else {
            owner.slot = g_freeSlots.back();
            g_freeSlots.pop_back();
            owner.fallback = false;
            SlotState& slot = g_slots[owner.slot];
            slot.key = chunkKey;
            slot.generation = owner.generation;
            slot.occupied = true;
        }
        try {
            g_chunkSlots.emplace(chunkKey, owner);
        }
        catch (...) {
            if (owner.slot != 0) {
                g_slots[owner.slot] = {};
                g_freeSlots.push_back(owner.slot);
            }
            return false;
        }
        ++g_stats.assignments;
        out = { owner.slot, owner.generation, owner.fallback };
        return true;
    }

    bool GetChunkSlot(uint64_t chunkKey, SlotRef& out) {
        const auto found = g_chunkSlots.find(chunkKey);
        if (found == g_chunkSlots.end()) return false;
        out = { found->second.slot, found->second.generation, found->second.fallback };
        return true;
    }

    bool SlotMatches(uint64_t chunkKey, uint16_t slot, uint64_t generation) {
        const auto found = g_chunkSlots.find(chunkKey);
        const bool matches = found != g_chunkSlots.end()
            && found->second.slot == slot && found->second.generation == generation;
        if (!matches) ++g_stats.generationRejects;
        return matches;
    }

    bool EnsureChunkTileUploaded(uint64_t chunkKey, uint16_t slot, uint64_t generation) {
        if (!SlotMatches(chunkKey, slot, generation)) return false;
        auto found = g_chunkSlots.find(chunkKey);
        if (found == g_chunkSlots.end()) return false;
        if (found->second.slot == 0) return true;
        std::unordered_set<uint64_t> keys{ chunkKey };
        return BuildAndUploadKeys(keys) && found->second.uploaded && !found->second.dirty;
    }

    bool RefreshChangedChunks(const std::vector<uint64_t>& changedChunkKeys) {
        if (changedChunkKeys.empty()) return true;
        std::unordered_set<uint64_t> affected;
        affected.reserve(changedChunkKeys.size() * 8 + 1);
        for (uint64_t key : changedChunkKeys) CollectAllocatedNeighborhood(key, affected);
        return BuildAndUploadKeys(affected);
    }

    bool RefreshAllAssigned() {
        std::unordered_set<uint64_t> assigned;
        assigned.reserve(g_chunkSlots.size());
        for (auto& pair : g_chunkSlots) {
            pair.second.dirty = true;
            assigned.insert(pair.first);
        }
        return BuildAndUploadKeys(assigned);
    }

    void MarkChunkNeighborhoodDirty(uint64_t chunkKey) {
        std::unordered_set<uint64_t> ignored;
        CollectAllocatedNeighborhood(chunkKey, ignored);
    }

    void ReleaseChunk(uint64_t chunkKey) {
        auto found = g_chunkSlots.find(chunkKey);
        if (found == g_chunkSlots.end()) return;
        const uint16_t slot = found->second.slot;
        g_chunkSlots.erase(found);
        if (slot != 0) {
            int originX = 0, originY = 0;
            SlotOrigin(slot, originX, originY);
            for (int row = 0; row < kTileHeight; ++row) {
                uint8_t* target = g_backing.data()
                    + (size_t(originY + row) * kAtlasSize + size_t(originX)) * 4;
                std::memset(target, kFullbrightPacked, size_t(kTileWidth) * 4);
            }
            g_slots[slot] = {};
            g_freeSlots.push_back(slot);
        }
        ++g_stats.releases;
    }

    void ClearWorld() {
        if (!g_initialized) return;
        std::fill(g_backing.begin(), g_backing.end(), kFullbrightPacked);
        ResetAllocator();
        g_stats = {};
        if (g_texture) {
            g_texture->Download();
            g_textureReady = true;
        }
    }

    void Shutdown() {
        if (g_texture) {
            g_texture->SetTextureRegenerator(nullptr);
            g_texture->Release();
        }
        g_texture = nullptr;
        g_textureReady = false;
        g_initialized = false;
        g_backing.clear();
        ResetAllocator();
        g_stats = {};
    }

    int BindLightAtlasMaterial(ILuaBase* LUA) {
        unsigned int length = 0;
        const char* name = LUA->GetString(1, &length);
        LUA->PushBool(name && length != 0 && BindMaterial(name));
        return 1;
    }

    int GetLightAtlasStats(ILuaBase* LUA) {
        LUA->CreateTable();
#define MC_ATLAS_NUMBER(name, value) LUA->PushNumber(double(value)); LUA->SetField(-2, name)
        MC_ATLAS_NUMBER("atlasSize", kAtlasSize);
        MC_ATLAS_NUMBER("tileWidth", kTileWidth);
        MC_ATLAS_NUMBER("tileHeight", kTileHeight);
        MC_ATLAS_NUMBER("slotCount", kSlotCount);
        MC_ATLAS_NUMBER("realCapacity", kRealCapacity);
        MC_ATLAS_NUMBER("assigned", g_chunkSlots.size());
        MC_ATLAS_NUMBER("free", g_freeSlots.size());
        MC_ATLAS_NUMBER("assignments", g_stats.assignments);
        MC_ATLAS_NUMBER("fallbackAssigned", g_stats.fallbackAssignments);
        MC_ATLAS_NUMBER("releases", g_stats.releases);
        MC_ATLAS_NUMBER("tileBuilds", g_stats.tileBuilds);
        MC_ATLAS_NUMBER("tileUploads", g_stats.tileUploads);
        MC_ATLAS_NUMBER("uploadRects", g_stats.uploadRects);
        MC_ATLAS_NUMBER("generationRejects", g_stats.generationRejects);
        MC_ATLAS_NUMBER("capacityRejects", g_stats.capacityRejects);
#undef MC_ATLAS_NUMBER
        LUA->PushBool(g_textureReady && g_texture != nullptr); LUA->SetField(-2, "textureReady");
        LUA->PushBool(true); LUA->SetField(-2, "stableSlots");
        return 1;
    }

    int GetLightAtlasSlot(ILuaBase* LUA) {
        int cx = 0, cy = 0, cz = 0;
        if (!worldstate::ReadChunkCoords(LUA, cx, cy, cz)) {
            LUA->PushNumber(0); LUA->PushNumber(0); return 2;
        }
        const uint64_t key = worldstate::PackChunkKey(cx, cy, cz);
        SlotRef ref;
        if (!EnsureChunkSlot(key, ref) || !EnsureChunkTileUploaded(key, ref.slot, ref.generation)) {
            LUA->PushNumber(0); LUA->PushNumber(0); return 2;
        }
        LUA->PushNumber(ref.slot);
        LUA->PushNumber(double(ref.generation));
        return 2;
    }
}
