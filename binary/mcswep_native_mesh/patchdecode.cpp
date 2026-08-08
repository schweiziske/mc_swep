#include "pch.h"
#include "patchdecode.h"
#include "worldstate.h"
#include "blockdefs.h"
#include "global.h"
#include "meshbuild.h"
#include "lightatlas.h"

#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace GarrysMod::Lua;

namespace mcmesh::patchdecode {
    namespace {
        constexpr size_t kValveLzmaHeaderBytes = 17;
        constexpr size_t kLzmaAloneHeaderBytes = 13;
        constexpr size_t kMaxCompressedBytes = 16u * 1024u * 1024u;
        constexpr size_t kMaxUncompressedBytes = 64u * 1024u * 1024u;
        constexpr size_t kMaxQueuedBytes = 32u * 1024u * 1024u;
        constexpr size_t kMaxReadyBytes = 192u * 1024u * 1024u;
        constexpr size_t kMaxJobs = 32;
        constexpr uint32_t kMaxChunks = 64;
        constexpr uint32_t kMaxCells = 131072;
        constexpr size_t kMaxPalette = 1048576;
        constexpr uint8_t kBinaryVersion = 1;
        constexpr uint8_t kBinarySections = 2;
        constexpr uint32_t kSectionCells = 4096;
        constexpr uint8_t kCompressionNone = 0;
        constexpr uint8_t kCompressionLzma = 1;

        enum class JobState { Queued, Running, Ready, Failed, Cancelled, Committed };
        enum class JobKind { JsonPatch, BinarySectionPatch };

        struct CellRef { uint16_t localIndex = 0; uint32_t paletteIndex = 0; };
        struct Chunk {
            int cx = 0, cy = 0, cz = 0;
            std::vector<CellRef> cells;
            std::string blockEntitiesJSON;
        };
        struct DecodedPatch {
            std::vector<std::string> palette;
            std::vector<Chunk> chunks;
            uint32_t cellCount = 0;
            size_t retainedBytes = 0;
            double decompressUs = 0, jsonUs = 0;
        };
        struct BinaryChunk {
            int cx = 0, cy = 0, cz = 0;
            uint32_t count = 0;
            std::array<uint32_t, kCellsPerChunk> states{};
            std::string blockEntitiesJSON;
        };
        struct DecodedBinaryPatch {
            std::vector<BinaryChunk> chunks;
            uint32_t cellCount = 0;
            size_t retainedBytes = 0;
            double decompressUs = 0, parseUs = 0;
        };
        struct Job {
            uint64_t id = 0, backendEpoch = 0;
            uint32_t generation = 0, revision = 0, declaredChunks = 0, declaredCells = 0;
            uint32_t rawBytes = 0;
            uint8_t compression = kCompressionLzma;
            JobKind kind = JobKind::JsonPatch;
            std::vector<uint8_t> compressed;
            DecodedPatch decoded;
            DecodedBinaryPatch binaryDecoded;
            JobState state = JobState::Queued;
            std::string error;
            bool cancelled = false;
        };

        std::mutex g_mutex;
        std::mutex g_apiMutex;
        std::condition_variable g_cv;
        std::deque<std::shared_ptr<Job>> g_queue;
        std::unordered_map<uint64_t, std::shared_ptr<Job>> g_jobs;
        std::thread g_thread;
        bool g_stopping = false;
        uint64_t g_nextJobId = 0;
        size_t g_queuedBytes = 0, g_readyBytes = 0;

        void* LzmaAlloc(ISzAllocPtr, size_t size) { return std::malloc(size); }
        void LzmaFree(ISzAllocPtr, void* address) { std::free(address); }
        ISzAlloc g_lzmaAllocator = { LzmaAlloc, LzmaFree };

        uint32_t ReadU32LE(const uint8_t* p) {
            return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
        }

        uint64_t ReadU64LE(const uint8_t* p) {
            uint64_t value = 0;
            for (int i = 0; i < 8; ++i) value |= uint64_t(p[i]) << (i * 8);
            return value;
        }

        struct BinaryReader {
            const uint8_t* data = nullptr;
            size_t size = 0, pos = 0;

            bool Bytes(size_t count, const uint8_t*& out) {
                if (pos > size || count > size - pos) return false;
                out = data + pos; pos += count; return true;
            }
            bool U8(uint8_t& out) {
                const uint8_t* p = nullptr; if (!Bytes(1, p)) return false; out = p[0]; return true;
            }
            bool U16(uint16_t& out) {
                const uint8_t* p = nullptr; if (!Bytes(2, p)) return false;
                out = uint16_t(p[0]) | uint16_t(uint16_t(p[1]) << 8); return true;
            }
            bool U32(uint32_t& out) {
                const uint8_t* p = nullptr; if (!Bytes(4, p)) return false; out = ReadU32LE(p); return true;
            }
            bool I32(int& out) {
                uint32_t value = 0; if (!U32(value)) return false;
                int32_t signedValue = 0; std::memcpy(&signedValue, &value, sizeof(value));
                out = int(signedValue); return true;
            }
        };

        int HexNibble(char c) {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }

        bool SchemaHashMatches(const rapidjson::Value& value) {
            if (!value.IsString() || value.GetStringLength() != 64) return false;
            const char* text = value.GetString();
            for (size_t i = 0; i < blockdefs::g_schemaHash.size(); ++i) {
                const int high = HexNibble(text[i * 2]), low = HexNibble(text[i * 2 + 1]);
                if (high < 0 || low < 0 || uint8_t((high << 4) | low) != blockdefs::g_schemaHash[i]) return false;
            }
            return true;
        }

        bool ReadUInt(const rapidjson::Value& value, uint32_t& out) {
            if (value.IsUint()) { out = value.GetUint(); return true; }
            if (value.IsUint64() && value.GetUint64() <= std::numeric_limits<uint32_t>::max()) {
                out = uint32_t(value.GetUint64()); return true;
            }
            return false;
        }

        bool ReadInt(const rapidjson::Value& value, int& out) {
            if (value.IsInt()) { out = value.GetInt(); return worldstate::ChunkCoordValid(out); }
            if (value.IsInt64() && value.GetInt64() >= -1000000 && value.GetInt64() <= 1000000) {
                out = int(value.GetInt64()); return true;
            }
            return false;
        }

        bool DecodeLzma(const std::vector<uint8_t>& input, std::vector<uint8_t>& output, std::string& error) {
            if (input.size() < kLzmaAloneHeaderBytes) { error = "invalid_lzma_header"; return false; }
            const bool valve = input.size() >= kValveLzmaHeaderBytes && std::memcmp(input.data(), "LZMA", 4) == 0;
            const size_t headerBytes = valve ? kValveLzmaHeaderBytes : kLzmaAloneHeaderBytes;
            const uint8_t* properties = input.data() + (valve ? 12 : 0);
            const uint32_t dictionarySize = ReadU32LE(properties + 1);
            uint64_t actualSize64 = valve ? ReadU32LE(input.data() + 4) : ReadU64LE(input.data() + 5);
            const size_t compressedSize = valve ? ReadU32LE(input.data() + 8) : input.size() - headerBytes;
            if (actualSize64 == 0 || actualSize64 > kMaxUncompressedBytes || dictionarySize > kMaxUncompressedBytes) {
                error = "invalid_uncompressed_size"; return false;
            }
            if (compressedSize != input.size() - headerBytes) {
                error = "invalid_compressed_size"; return false;
            }
            const size_t actualSize = size_t(actualSize64);
            try { output.resize(actualSize); }
            catch (...) { error = "decode_allocation_failed"; return false; }
            SizeT outSize = actualSize, inSize = compressedSize;
            ELzmaStatus status = LZMA_STATUS_NOT_SPECIFIED;
            const SRes result = LzmaDecode(output.data(), &outSize,
                input.data() + headerBytes, &inSize, properties,
                LZMA_PROPS_SIZE, LZMA_FINISH_END, &status, &g_lzmaAllocator);
            if (result != SZ_OK || outSize != actualSize || inSize != compressedSize
                || (status != LZMA_STATUS_FINISHED_WITH_MARK && status != LZMA_STATUS_MAYBE_FINISHED_WITHOUT_MARK)) {
                output.clear(); error = "lzma_decode_failed"; return false;
            }
            return true;
        }

        bool ParseCells(const char* data, size_t size, std::vector<CellRef>& out, uint32_t& total, std::string& error) {
            if (size == 0) return true;
            size_t pos = 0;
            std::vector<uint8_t> seen(kCellsPerChunk, 0);
            while (pos < size) {
                uint64_t li = 0, palette = 0;
                size_t digits = 0;
                while (pos < size && data[pos] >= '0' && data[pos] <= '9') {
                    li = li * 10 + uint64_t(data[pos++] - '0'); ++digits;
                    if (li >= uint64_t(kCellsPerChunk)) { error = "invalid_local_index"; return false; }
                }
                if (!digits || pos >= size || data[pos++] != ':') { error = "invalid_cell_string"; return false; }
                digits = 0;
                while (pos < size && data[pos] >= '0' && data[pos] <= '9') {
                    palette = palette * 10 + uint64_t(data[pos++] - '0'); ++digits;
                    if (palette > std::numeric_limits<uint32_t>::max()) { error = "invalid_palette_index"; return false; }
                }
                if (!digits || seen[size_t(li)] || total >= kMaxCells) { error = "invalid_or_duplicate_cell"; return false; }
                seen[size_t(li)] = 1;
                out.push_back({ uint16_t(li), uint32_t(palette) });
                ++total;
                if (pos == size) break;
                if (data[pos++] != ';' || pos == size) { error = "invalid_cell_separator"; return false; }
            }
            return true;
        }

        bool SerializeValue(const rapidjson::Value& value, std::string& out) {
            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            if (!value.Accept(writer)) return false;
            out.assign(buffer.GetString(), buffer.GetSize());
            return true;
        }

        bool ParsePatch(const std::vector<uint8_t>& json, const Job& job, DecodedPatch& out, std::string& error) {
            rapidjson::Document root;
            root.Parse(reinterpret_cast<const char*>(json.data()), json.size());
            if (root.HasParseError() || !root.IsObject()) { error = "invalid_json"; return false; }
            const auto schemaHashIt = root.FindMember("schemaHash");
            const auto chunkPatch = root.FindMember("chunkPatch");
            const auto chunksIt = root.FindMember("chunks");
            const auto paletteIt = root.FindMember("p");
            const auto blockCountIt = root.FindMember("blockCount");
            const auto csIt = root.FindMember("cs");
            const auto hIt = root.FindMember("h");
            if (schemaHashIt == root.MemberEnd() || !SchemaHashMatches(schemaHashIt->value)
                || chunkPatch == root.MemberEnd() || !chunkPatch->value.IsBool() || !chunkPatch->value.GetBool()
                || chunksIt == root.MemberEnd() || !chunksIt->value.IsArray()
                || paletteIt == root.MemberEnd() || !paletteIt->value.IsArray()
                || blockCountIt == root.MemberEnd() || csIt == root.MemberEnd() || hIt == root.MemberEnd()) {
                error = "invalid_patch_schema"; return false;
            }
            uint32_t blockCount = 0, cs = 0, ch = 0;
            if (!ReadUInt(blockCountIt->value, blockCount) || !ReadUInt(csIt->value, cs) || !ReadUInt(hIt->value, ch)
                || cs != uint32_t(g_promise.CS) || ch != uint32_t(g_promise.CH)
                || chunksIt->value.Size() != job.declaredChunks || blockCount != job.declaredCells
                || paletteIt->value.Size() > kMaxPalette) {
                error = "patch_metadata_mismatch"; return false;
            }
            try {
                out.palette.reserve(paletteIt->value.Size());
                for (const auto& value : paletteIt->value.GetArray()) {
                    if (!value.IsString() || value.GetStringLength() == 0 || value.GetStringLength() > 4096) {
                        error = "invalid_palette"; return false;
                    }
                    out.palette.emplace_back(value.GetString(), value.GetStringLength());
                }
                out.chunks.reserve(chunksIt->value.Size());
                std::unordered_set<uint64_t> seenChunks;
                for (const auto& value : chunksIt->value.GetArray()) {
                    if (!value.IsArray() || value.Size() < 4 || value.Size() > 6) { error = "invalid_chunk_record"; return false; }
                    Chunk chunk;
                    if (!ReadInt(value[0], chunk.cx) || !ReadInt(value[1], chunk.cy) || !ReadInt(value[2], chunk.cz)
                        || !value[3].IsString()) { error = "invalid_chunk_record"; return false; }
                    if (!seenChunks.insert(worldstate::PackChunkKey(chunk.cx, chunk.cy, chunk.cz)).second) {
                        error = "duplicate_chunk"; return false;
                    }
                    if (!ParseCells(value[3].GetString(), value[3].GetStringLength(), chunk.cells, out.cellCount, error)) return false;
                    if (value.Size() >= 5 && !value[4].IsNull()) {
                        if (!value[4].IsArray() || !SerializeValue(value[4], chunk.blockEntitiesJSON)) {
                            error = "invalid_block_entities"; return false;
                        }
                    }
                    if (value.Size() >= 6 && !value[5].IsNull()) { error = "unexpected_opaque_block_entities"; return false; }
                    for (const CellRef& cell : chunk.cells)
                        if (cell.paletteIndex >= out.palette.size()) { error = "palette_index_out_of_range"; return false; }
                    out.chunks.push_back(std::move(chunk));
                }
            }
            catch (...) { error = "json_allocation_failed"; return false; }
            if (out.cellCount != blockCount) { error = "cell_count_mismatch"; return false; }
            size_t retained = 0;
            for (const auto& s : out.palette) retained += s.size();
            for (const auto& c : out.chunks) retained += sizeof(Chunk) + c.cells.size() * sizeof(CellRef) + c.blockEntitiesJSON.size();
            out.retainedBytes = retained;
            return true;
        }

        bool ValidBinaryState(uint32_t stateId) {
            return stateId < blockdefs::g_states.size();
        }

        bool ParseBinarySection(BinaryReader& reader, uint32_t* states, uint32_t& count, std::string& error) {
            uint8_t mode = 0;
            if (!reader.U8(mode)) { error = "truncated_section_mode"; return false; }
            if (mode == 0) { count = 0; return true; }

            uint16_t declaredCount = 0;
            if (!reader.U16(declaredCount) || declaredCount == 0 || declaredCount > kSectionCells) {
                error = "invalid_section_count"; return false;
            }
            if (mode == 1) {
                uint32_t stateId = 0;
                if (!reader.U32(stateId) || declaredCount != kSectionCells || stateId == 0 || !ValidBinaryState(stateId)) {
                    error = "invalid_uniform_section"; return false;
                }
                std::fill_n(states, kSectionCells, stateId);
                count = kSectionCells;
                return true;
            }
            if (mode != 2) { error = "invalid_section_mode"; return false; }

            uint8_t bits = 0;
            uint16_t paletteSize = 0;
            if (!reader.U8(bits) || !reader.U16(paletteSize) || bits < 1 || bits > 12
                || paletteSize < 2 || paletteSize > kSectionCells || paletteSize > (1u << bits)) {
                error = "invalid_packed_section"; return false;
            }
            std::vector<uint32_t> palette;
            std::vector<uint32_t> words;
            try { palette.resize(paletteSize); }
            catch (...) { error = "binary_palette_allocation_failed"; return false; }
            std::unordered_set<uint32_t> seenPalette;
            for (uint32_t i = 0; i < paletteSize; ++i) {
                if (!reader.U32(palette[i]) || !ValidBinaryState(palette[i]) || !seenPalette.insert(palette[i]).second) {
                    error = "invalid_section_palette"; return false;
                }
            }
            uint16_t wordCount = 0;
            const uint32_t expectedWords = (kSectionCells * uint32_t(bits) + 31u) / 32u;
            if (!reader.U16(wordCount) || wordCount != expectedWords) {
                error = "invalid_section_word_count"; return false;
            }
            try { words.resize(wordCount); }
            catch (...) { error = "binary_words_allocation_failed"; return false; }
            for (uint32_t i = 0; i < wordCount; ++i)
                if (!reader.U32(words[i])) { error = "truncated_section_words"; return false; }

            const uint64_t mask = (uint64_t(1) << bits) - 1;
            uint32_t actualCount = 0;
            for (uint32_t cell = 0; cell < kSectionCells; ++cell) {
                const uint32_t bitOffset = cell * uint32_t(bits);
                const uint32_t wordIndex = bitOffset >> 5;
                const uint32_t shift = bitOffset & 31u;
                uint64_t packed = words[wordIndex];
                if (shift + bits > 32) packed |= uint64_t(words[wordIndex + 1]) << 32;
                const uint32_t paletteIndex = uint32_t((packed >> shift) & mask);
                if (paletteIndex >= paletteSize) { error = "section_palette_index_out_of_range"; return false; }
                const uint32_t stateId = palette[paletteIndex];
                states[cell] = stateId;
                if (stateId != 0) ++actualCount;
            }
            if (actualCount != declaredCount) { error = "section_count_mismatch"; return false; }
            count = actualCount;
            return true;
        }

        bool ParseBinaryBlockEntities(BinaryReader& reader, uint16_t entityCount, const Job& job,
            const BinaryChunk& chunk, std::string& output, std::string& error) {
            if (entityCount == 0) return true;
            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            std::vector<uint8_t> seen(kCellsPerChunk, 0);
            writer.StartArray();
            for (uint32_t i = 0; i < entityCount; ++i) {
                uint16_t localIndex = 0, typeLength = 0, codecVersion = 0, payloadLength = 0;
                const uint8_t* type = nullptr;
                const uint8_t* payload = nullptr;
                if (!reader.U16(localIndex) || !reader.U16(typeLength) || typeLength == 0 || typeLength > 160
                    || localIndex >= kCellsPerChunk || seen[localIndex] || !reader.Bytes(typeLength, type)
                    || !reader.U16(codecVersion) || !reader.U16(payloadLength) || payloadLength > 32768
                    || !reader.Bytes(payloadLength, payload)) {
                    error = "invalid_block_entity_header"; return false;
                }
                rapidjson::Document envelopePayload;
                envelopePayload.Parse(reinterpret_cast<const char*>(payload), payloadLength);
                if (!envelopePayload.IsObject()) { error = "invalid_block_entity_payload"; return false; }
                const auto hasData = envelopePayload.FindMember("hasData");
                if (hasData == envelopePayload.MemberEnd() || !hasData->value.IsBool()) {
                    error = "invalid_block_entity_payload"; return false;
                }
                const auto data = envelopePayload.FindMember("data");
                if (hasData->value.GetBool() && data == envelopePayload.MemberEnd()) {
                    error = "invalid_block_entity_payload"; return false;
                }
                if (chunk.states[localIndex] == 0) { error = "block_entity_on_air"; return false; }
                seen[localIndex] = 1;
                writer.StartObject();
                writer.Key("localIndex"); writer.Uint(localIndex);
                writer.Key("type"); writer.String(reinterpret_cast<const char*>(type), typeLength);
                writer.Key("codecVersion"); writer.Uint(codecVersion);
                if (hasData->value.GetBool()) { writer.Key("data"); data->value.Accept(writer); }
                writer.Key("revision"); writer.Uint(job.revision);
                writer.EndObject();
            }
            writer.EndArray();
            output.assign(buffer.GetString(), buffer.GetSize());
            return true;
        }

        bool ParseBinaryPatch(const std::vector<uint8_t>& raw, const Job& job,
            DecodedBinaryPatch& out, std::string& error) {
            if (raw.size() < 14) { error = "invalid_binary_patch_payload"; return false; }
            auto definitionsLock = blockdefs::LockForRead();
            BinaryReader reader{ raw.data(), raw.size(), 0 };
            const uint8_t* magic = nullptr;
            uint8_t version = 0, cs = 0, ch = 0, sectionCount = 0;
            uint16_t chunkCount = 0;
            uint32_t declaredCells = 0;
            if (!reader.Bytes(4, magic) || std::memcmp(magic, "MCB9", 4) != 0) {
                error = "binary_patch_magic_mismatch"; return false;
            }
            if (!reader.U8(version) || version != kBinaryVersion) { error = "binary_patch_version_mismatch"; return false; }
            if (!reader.U8(cs) || !reader.U8(ch) || !reader.U8(sectionCount)
                || cs != uint8_t(g_promise.CS) || ch != uint8_t(g_promise.CH) || sectionCount != kBinarySections) {
                error = "binary_patch_geometry_mismatch"; return false;
            }
            if (!reader.U16(chunkCount) || !reader.U32(declaredCells) || chunkCount == 0 || chunkCount > kMaxChunks
                || chunkCount != job.declaredChunks || declaredCells != job.declaredCells) {
                error = "binary_patch_metadata_mismatch"; return false;
            }

            try { out.chunks.reserve(chunkCount); }
            catch (...) { error = "binary_chunk_allocation_failed"; return false; }
            std::unordered_set<uint64_t> seenChunks;
            for (uint32_t chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex) {
                BinaryChunk chunk;
                uint8_t flags = 0;
                uint16_t blockEntityCount = 0;
                if (!reader.I32(chunk.cx) || !reader.I32(chunk.cy) || !reader.I32(chunk.cz)
                    || !worldstate::ChunkCoordValid(chunk.cx) || !worldstate::ChunkCoordValid(chunk.cy)
                    || !worldstate::ChunkCoordValid(chunk.cz) || !reader.U8(flags) || flags > 1
                    || !reader.U16(blockEntityCount) || blockEntityCount > kCellsPerChunk) {
                    error = "invalid_binary_chunk_header"; return false;
                }
                const uint64_t key = worldstate::PackChunkKey(chunk.cx, chunk.cy, chunk.cz);
                if (!seenChunks.insert(key).second) { error = "duplicate_binary_chunk"; return false; }
                if (flags == 1) {
                    if (blockEntityCount != 0) { error = "tombstone_block_entities"; return false; }
                }
                else {
                    for (uint32_t section = 0; section < kBinarySections; ++section) {
                        uint32_t sectionCells = 0;
                        if (!ParseBinarySection(reader, chunk.states.data() + section * kSectionCells, sectionCells, error)) {
                            error += ":" + std::to_string(chunkIndex + 1) + ":" + std::to_string(section + 1);
                            return false;
                        }
                        chunk.count += sectionCells;
                    }
                    if (!ParseBinaryBlockEntities(reader, blockEntityCount, job, chunk, chunk.blockEntitiesJSON, error)) {
                        error += ":" + std::to_string(chunkIndex + 1); return false;
                    }
                }
                if (chunk.count > kMaxCells - out.cellCount) { error = "binary_patch_cell_limit"; return false; }
                out.cellCount += chunk.count;
                out.chunks.push_back(std::move(chunk));
            }
            if (out.cellCount != declaredCells) { error = "binary_patch_total_count_mismatch"; return false; }
            if (reader.pos != reader.size) { error = "binary_patch_trailing_data"; return false; }
            size_t retained = 0;
            for (const auto& chunk : out.chunks) retained += sizeof(BinaryChunk) + chunk.blockEntitiesJSON.size();
            out.retainedBytes = retained;
            return true;
        }

        void WorkerMain() {
            for (;;) {
                std::shared_ptr<Job> job;
                {
                    std::unique_lock<std::mutex> lock(g_mutex);
                    g_cv.wait(lock, [] { return g_stopping || !g_queue.empty(); });
                    if (g_stopping && g_queue.empty()) return;
                    job = g_queue.front(); g_queue.pop_front();
                    g_queuedBytes -= job->compressed.size();
                    if (job->cancelled) { job->state = JobState::Cancelled; continue; }
                    job->state = JobState::Running;
                }
                std::vector<uint8_t> raw;
                std::string error;
                const auto decompressStart = std::chrono::steady_clock::now();
                bool ok = true;
                if (job->compression == kCompressionLzma) ok = DecodeLzma(job->compressed, raw, error);
                else raw = std::move(job->compressed);
                const double decompressUs = std::chrono::duration<double, std::micro>(
                    std::chrono::steady_clock::now() - decompressStart).count();
                if (ok && job->rawBytes != 0 && raw.size() != job->rawBytes) {
                    error = "binary_patch_raw_size_mismatch"; ok = false;
                }
                if (job->kind == JobKind::BinarySectionPatch) job->binaryDecoded.decompressUs = decompressUs;
                else job->decoded.decompressUs = decompressUs;
                if (ok) {
                    const auto parseStart = std::chrono::steady_clock::now();
                    if (job->kind == JobKind::BinarySectionPatch) {
                        ok = ParseBinaryPatch(raw, *job, job->binaryDecoded, error);
                        job->binaryDecoded.parseUs = std::chrono::duration<double, std::micro>(
                            std::chrono::steady_clock::now() - parseStart).count();
                    }
                    else {
                        ok = ParsePatch(raw, *job, job->decoded, error);
                        job->decoded.jsonUs = std::chrono::duration<double, std::micro>(
                            std::chrono::steady_clock::now() - parseStart).count();
                    }
                }
                job->compressed.clear(); job->compressed.shrink_to_fit();
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    const size_t retainedBytes = job->kind == JobKind::BinarySectionPatch
                        ? job->binaryDecoded.retainedBytes : job->decoded.retainedBytes;
                    if (job->cancelled || g_stopping) job->state = JobState::Cancelled;
                    else if (!ok) { job->error = error; job->state = JobState::Failed; }
                    else if (retainedBytes > kMaxReadyBytes - std::min(g_readyBytes, kMaxReadyBytes)) {
                        job->error = "ready_queue_full"; job->decoded = {}; job->binaryDecoded = {}; job->state = JobState::Failed;
                    }
                    else { g_readyBytes += retainedBytes; job->state = JobState::Ready; }
                }
            }
        }

        bool StartWorker() {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_thread.joinable()) return true;
            g_stopping = false;
            try { g_thread = std::thread(WorkerMain); }
            catch (...) { return false; }
            return true;
        }

        bool ReadExactUInt(ILuaBase* LUA, int index, uint64_t max, uint64_t& out) {
            if (!LUA->IsType(index, Type::Number)) return false;
            const double value = LUA->GetNumber(index);
            if (!std::isfinite(value) || value < 0 || value > double(max) || value != std::floor(value)) return false;
            out = uint64_t(value); return true;
        }

        std::shared_ptr<Job> FindJob(uint64_t id) {
            const auto it = g_jobs.find(id);
            return it == g_jobs.end() ? nullptr : it->second;
        }

        void PushStatus(ILuaBase* LUA, const char* status) {
            LUA->CreateTable(); LUA->PushString(status); LUA->SetField(-2, "status");
        }

        void RemoveJobLocked(const std::shared_ptr<Job>& job) {
            const size_t retainedBytes = job->kind == JobKind::BinarySectionPatch
                ? job->binaryDecoded.retainedBytes : job->decoded.retainedBytes;
            if (job->state == JobState::Ready) g_readyBytes -= std::min(g_readyBytes, retainedBytes);
            g_jobs.erase(job->id);
        }
    }

    int StartCompressedPatchDecode(ILuaBase* LUA) {
        if (!LUA->IsType(1, Type::String)) { LUA->PushBool(false); return 1; }
        unsigned int size = 0; const char* data = LUA->GetString(1, &size);
        uint64_t generation=0,revision=0,epoch=0,chunks=0,cells=0;
        if (!data || size < kLzmaAloneHeaderBytes || size > kMaxCompressedBytes
            || !ReadExactUInt(LUA,2,UINT32_MAX,generation) || !ReadExactUInt(LUA,3,UINT32_MAX,revision)
            || !ReadExactUInt(LUA,4,9007199254740991ull,epoch) || !ReadExactUInt(LUA,5,kMaxChunks,chunks)
            || !ReadExactUInt(LUA,6,kMaxCells,cells) || chunks == 0 || !StartWorker()) {
            LUA->PushBool(false); return 1;
        }
        auto job = std::make_shared<Job>();
        try { job->compressed.assign(reinterpret_cast<const uint8_t*>(data), reinterpret_cast<const uint8_t*>(data) + size); }
        catch (...) { LUA->PushBool(false); return 1; }
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_jobs.size() >= kMaxJobs || size > kMaxQueuedBytes - std::min(g_queuedBytes, kMaxQueuedBytes)) {
                LUA->PushBool(false); return 1;
            }
            job->id = ++g_nextJobId; if (!job->id) job->id = ++g_nextJobId;
            job->generation=uint32_t(generation);job->revision=uint32_t(revision);job->backendEpoch=epoch;
            job->declaredChunks=uint32_t(chunks);job->declaredCells=uint32_t(cells);
            g_queuedBytes += size; g_jobs[job->id] = job; g_queue.push_back(job);
        }
        g_cv.notify_one(); LUA->PushNumber(double(job->id)); return 1;
    }

    int StartBinarySectionPatchDecode(ILuaBase* LUA) {
        if (!LUA->IsType(1, Type::String)) { LUA->PushBool(false); return 1; }
        unsigned int size = 0; const char* data = LUA->GetString(1, &size);
        uint64_t compression=0,rawBytes=0,generation=0,revision=0,epoch=0,chunks=0,cells=0;
        if (!data || size == 0 || size > kMaxCompressedBytes
            || !ReadExactUInt(LUA,2,kCompressionLzma,compression)
            || !ReadExactUInt(LUA,3,kMaxUncompressedBytes,rawBytes) || rawBytes < 14
            || !ReadExactUInt(LUA,4,UINT32_MAX,generation) || !ReadExactUInt(LUA,5,UINT32_MAX,revision)
            || !ReadExactUInt(LUA,6,9007199254740991ull,epoch) || !ReadExactUInt(LUA,7,kMaxChunks,chunks)
            || !ReadExactUInt(LUA,8,kMaxCells,cells) || chunks == 0
            || (compression == kCompressionNone && size != rawBytes)
            || !blockdefs::Seal() || !StartWorker()) {
            LUA->PushBool(false); return 1;
        }
        auto job = std::make_shared<Job>();
        try { job->compressed.assign(reinterpret_cast<const uint8_t*>(data), reinterpret_cast<const uint8_t*>(data) + size); }
        catch (...) { LUA->PushBool(false); return 1; }
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_jobs.size() >= kMaxJobs || size > kMaxQueuedBytes - std::min(g_queuedBytes, kMaxQueuedBytes)) {
                LUA->PushBool(false); return 1;
            }
            job->id = ++g_nextJobId; if (!job->id) job->id = ++g_nextJobId;
            job->kind=JobKind::BinarySectionPatch;job->compression=uint8_t(compression);job->rawBytes=uint32_t(rawBytes);
            job->generation=uint32_t(generation);job->revision=uint32_t(revision);job->backendEpoch=epoch;
            job->declaredChunks=uint32_t(chunks);job->declaredCells=uint32_t(cells);
            g_queuedBytes += size; g_jobs[job->id] = job; g_queue.push_back(job);
        }
        g_cv.notify_one(); LUA->PushNumber(double(job->id)); return 1;
    }

    int PollJob(ILuaBase* LUA, JobKind expectedKind) {
        uint64_t id=0;if(!ReadExactUInt(LUA,1,9007199254740991ull,id)){LUA->PushNil();return 1;}
        std::lock_guard<std::mutex> lock(g_mutex);auto job=FindJob(id);if(!job||job->kind!=expectedKind){LUA->PushNil();return 1;}
        if(job->state==JobState::Queued||job->state==JobState::Running){PushStatus(LUA,"pending");return 1;}
        if(job->state==JobState::Failed){PushStatus(LUA,"failed");LUA->PushString(job->error.c_str());LUA->SetField(-2,"error");return 1;}
        if(job->state==JobState::Cancelled){PushStatus(LUA,"cancelled");return 1;}
        if(job->state!=JobState::Ready){PushStatus(LUA,"consumed");return 1;}
        PushStatus(LUA,"ready");
        LUA->PushNumber(job->generation);LUA->SetField(-2,"generation");LUA->PushNumber(job->revision);LUA->SetField(-2,"revision");
        LUA->PushNumber(double(job->backendEpoch));LUA->SetField(-2,"backendEpoch");
        if(expectedKind==JobKind::BinarySectionPatch){
            LUA->PushBool(true);LUA->SetField(-2,"binary");LUA->PushNumber(job->binaryDecoded.cellCount);LUA->SetField(-2,"cells");
            LUA->CreateTable();int n=1;for(const auto& chunk:job->binaryDecoded.chunks){LUA->PushNumber(n++);LUA->CreateTable();
                LUA->PushNumber(chunk.cx);LUA->SetField(-2,"cx");LUA->PushNumber(chunk.cy);LUA->SetField(-2,"cy");LUA->PushNumber(chunk.cz);LUA->SetField(-2,"cz");
                LUA->PushNumber(chunk.count);LUA->SetField(-2,"count");LUA->PushBool(!chunk.blockEntitiesJSON.empty());LUA->SetField(-2,"hasBlockEntities");LUA->SetTable(-3);}LUA->SetField(-2,"chunks");
        }else{
            LUA->PushNumber(job->decoded.cellCount);LUA->SetField(-2,"cells");
            LUA->CreateTable();int n=1;for(const auto& spec:job->decoded.palette){LUA->PushNumber(n++);LUA->PushString(spec.c_str(),(unsigned int)spec.size());LUA->SetTable(-3);}LUA->SetField(-2,"palette");
            LUA->CreateTable();n=1;for(const auto& chunk:job->decoded.chunks){LUA->PushNumber(n++);LUA->CreateTable();
                LUA->PushNumber(chunk.cx);LUA->SetField(-2,"cx");LUA->PushNumber(chunk.cy);LUA->SetField(-2,"cy");LUA->PushNumber(chunk.cz);LUA->SetField(-2,"cz");
                LUA->PushNumber(double(chunk.cells.size()));LUA->SetField(-2,"count");LUA->PushBool(!chunk.blockEntitiesJSON.empty());LUA->SetField(-2,"hasBlockEntities");LUA->SetTable(-3);}LUA->SetField(-2,"chunks");
        }
        return 1;
    }

    int PollCompressedPatchDecode(ILuaBase* LUA) {
        return PollJob(LUA, JobKind::JsonPatch);
    }

    int PollBinarySectionPatchDecode(ILuaBase* LUA) { return PollJob(LUA, JobKind::BinarySectionPatch); }

    int CommitCompressedPatch(ILuaBase* LUA) {
        std::lock_guard<std::mutex> apiLock(g_apiMutex);
        uint64_t id=0;if(!ReadExactUInt(LUA,1,9007199254740991ull,id)||!LUA->IsType(2,Type::Table)||!LUA->IsType(3,Type::Table)){LUA->PushBool(false);return 1;}
        std::shared_ptr<Job> job;{
            std::lock_guard<std::mutex> lock(g_mutex);job=FindJob(id);if(!job||job->kind!=JobKind::JsonPatch||job->state!=JobState::Ready){LUA->PushBool(false);return 1;}
        }
        if(g_promise.SS<=0||!blockdefs::Seal()||LUA->ObjLen(2)!=(int)job->decoded.palette.size()){LUA->PushBool(false);return 1;}
        std::vector<uint32_t> palette(job->decoded.palette.size());
        for(int i=1;i<=LUA->ObjLen(2);++i){LUA->PushNumber(i);LUA->RawGet(2);if(!LUA->IsType(-1,Type::Number)){LUA->Pop(1);LUA->PushBool(false);return 1;}const double v=LUA->GetNumber(-1);LUA->Pop(1);if(v!=floor(v)||v<=0||v>=blockdefs::g_states.size()){LUA->PushBool(false);return 1;}palette[i-1]=uint32_t(v);}
        std::vector<uint8_t> include(job->decoded.chunks.size(),0);const int includeCount=LUA->ObjLen(3);
        for(int i=1;i<=includeCount;++i){LUA->PushNumber(i);LUA->RawGet(3);if(!LUA->IsType(-1,Type::Number)){LUA->Pop(1);LUA->PushBool(false);return 1;}const double v=LUA->GetNumber(-1);LUA->Pop(1);if(v!=floor(v)||v<1||v>include.size()||include[size_t(v)-1]){LUA->PushBool(false);return 1;}include[size_t(v)-1]=1;}
        struct PreparedCommit {
            const Chunk* chunk = nullptr;
            std::array<uint32_t, kCellsPerChunk> states{};
        };
        std::vector<PreparedCommit> prepared;
        try { prepared.reserve(includeCount); }
        catch (...) { LUA->PushBool(false); return 1; }
        uint32_t blocks=0;
        for(size_t ci=0;ci<job->decoded.chunks.size();++ci)if(include[ci]){
            prepared.emplace_back(); auto& target=prepared.back(); target.chunk=&job->decoded.chunks[ci];
            for(const auto& cell:target.chunk->cells){target.states[cell.localIndex]=palette[cell.paletteIndex];++blocks;}
        }
        std::vector<uint64_t> changedKeys;
        changedKeys.reserve(prepared.size());
        for(const auto& target:prepared){
            if(!worldstate::ApplyDenseStates(target.chunk->cx,target.chunk->cy,target.chunk->cz,target.states.data())){LUA->PushBool(false);return 1;}
            changedKeys.push_back(worldstate::PackChunkKey(target.chunk->cx,target.chunk->cy,target.chunk->cz));
        }
        lightatlas::RefreshChangedChunks(changedKeys);
        for(const auto& target:prepared)meshbuild::RefreshChunkDrawMetadata(
            worldstate::PackChunkKey(target.chunk->cx,target.chunk->cy,target.chunk->cz));
        meshbuild::InvalidateDirtyOcclusionMetadata();
        LUA->CreateTable();LUA->PushNumber(blocks);LUA->SetField(-2,"blocks");LUA->PushNumber(job->decoded.decompressUs);LUA->SetField(-2,"decompressUs");LUA->PushNumber(job->decoded.jsonUs);LUA->SetField(-2,"jsonUs");
        LUA->CreateTable();int out=1;for(size_t ci=0;ci<job->decoded.chunks.size();++ci)if(include[ci]){const auto& c=job->decoded.chunks[ci];LUA->PushNumber(out++);LUA->CreateTable();LUA->PushNumber(c.cx);LUA->SetField(-2,"cx");LUA->PushNumber(c.cy);LUA->SetField(-2,"cy");LUA->PushNumber(c.cz);LUA->SetField(-2,"cz");LUA->PushNumber(double(c.cells.size()));LUA->SetField(-2,"count");if(!c.blockEntitiesJSON.empty()){LUA->PushString(c.blockEntitiesJSON.c_str(),(unsigned int)c.blockEntitiesJSON.size());LUA->SetField(-2,"blockEntitiesJSON");}LUA->SetTable(-3);}LUA->SetField(-2,"chunks");
        {std::lock_guard<std::mutex> lock(g_mutex);RemoveJobLocked(job);job->state=JobState::Committed;}return 1;
    }

    int CommitBinarySectionPatch(ILuaBase* LUA) {
        std::lock_guard<std::mutex> apiLock(g_apiMutex);
        uint64_t id=0;if(!ReadExactUInt(LUA,1,9007199254740991ull,id)||!LUA->IsType(2,Type::Table)){LUA->PushBool(false);return 1;}
        std::shared_ptr<Job> job;{
            std::lock_guard<std::mutex> lock(g_mutex);job=FindJob(id);
            if(!job||job->kind!=JobKind::BinarySectionPatch||job->state!=JobState::Ready){LUA->PushBool(false);return 1;}
        }
        std::vector<uint8_t> include(job->binaryDecoded.chunks.size(),0);const int includeCount=LUA->ObjLen(2);
        for(int i=1;i<=includeCount;++i){LUA->PushNumber(i);LUA->RawGet(2);if(!LUA->IsType(-1,Type::Number)){LUA->Pop(1);LUA->PushBool(false);return 1;}const double v=LUA->GetNumber(-1);LUA->Pop(1);if(v!=floor(v)||v<1||v>include.size()||include[size_t(v)-1]){LUA->PushBool(false);return 1;}include[size_t(v)-1]=1;}
        uint32_t blocks=0;
        std::vector<uint64_t> changedKeys;
        changedKeys.reserve(includeCount);
        for(size_t ci=0;ci<job->binaryDecoded.chunks.size();++ci)if(include[ci]){
            const auto& chunk=job->binaryDecoded.chunks[ci];
            if(!worldstate::ApplyDenseStates(chunk.cx,chunk.cy,chunk.cz,chunk.states.data())){LUA->PushBool(false);return 1;}
            blocks+=chunk.count;
            changedKeys.push_back(worldstate::PackChunkKey(chunk.cx,chunk.cy,chunk.cz));
        }
        lightatlas::RefreshChangedChunks(changedKeys);
        for(size_t ci=0;ci<job->binaryDecoded.chunks.size();++ci)if(include[ci]){
            const auto& chunk=job->binaryDecoded.chunks[ci];
            meshbuild::RefreshChunkDrawMetadata(worldstate::PackChunkKey(chunk.cx,chunk.cy,chunk.cz));
        }
        meshbuild::InvalidateDirtyOcclusionMetadata();
        LUA->CreateTable();LUA->PushNumber(blocks);LUA->SetField(-2,"blocks");
        LUA->PushNumber(job->binaryDecoded.decompressUs);LUA->SetField(-2,"decompressUs");
        LUA->PushNumber(job->binaryDecoded.parseUs);LUA->SetField(-2,"parseUs");
        LUA->CreateTable();int out=1;for(size_t ci=0;ci<job->binaryDecoded.chunks.size();++ci)if(include[ci]){const auto& c=job->binaryDecoded.chunks[ci];LUA->PushNumber(out++);LUA->CreateTable();
            LUA->PushNumber(c.cx);LUA->SetField(-2,"cx");LUA->PushNumber(c.cy);LUA->SetField(-2,"cy");LUA->PushNumber(c.cz);LUA->SetField(-2,"cz");LUA->PushNumber(c.count);LUA->SetField(-2,"count");
            if(!c.blockEntitiesJSON.empty()){LUA->PushString(c.blockEntitiesJSON.c_str(),(unsigned int)c.blockEntitiesJSON.size());LUA->SetField(-2,"blockEntitiesJSON");}LUA->SetTable(-3);}LUA->SetField(-2,"chunks");
        {std::lock_guard<std::mutex> lock(g_mutex);RemoveJobLocked(job);job->state=JobState::Committed;}return 1;
    }

    int CancelCompressedPatchDecode(ILuaBase* LUA) {
        std::lock_guard<std::mutex> apiLock(g_apiMutex);
        uint64_t id=0;if(!ReadExactUInt(LUA,1,9007199254740991ull,id)){LUA->PushBool(false);return 1;}std::lock_guard<std::mutex> lock(g_mutex);auto job=FindJob(id);if(!job){LUA->PushBool(false);return 1;}job->cancelled=true;if(job->state==JobState::Ready){const size_t retained=job->kind==JobKind::BinarySectionPatch?job->binaryDecoded.retainedBytes:job->decoded.retainedBytes;g_readyBytes-=std::min(g_readyBytes,retained);}g_jobs.erase(id);LUA->PushBool(true);return 1;
    }

    int CancelAllCompressedPatchDecodes(ILuaBase* LUA) { CancelAll();LUA->PushBool(true);return 1; }

    void CancelAll() {
        std::lock_guard<std::mutex> apiLock(g_apiMutex);
        std::lock_guard<std::mutex> lock(g_mutex);for(auto& pair:g_jobs)pair.second->cancelled=true;g_jobs.clear();g_queue.clear();g_queuedBytes=0;g_readyBytes=0;
    }

    void Shutdown() {
        std::lock_guard<std::mutex> apiLock(g_apiMutex);
        {std::lock_guard<std::mutex> lock(g_mutex);g_stopping=true;for(auto& pair:g_jobs)pair.second->cancelled=true;g_queue.clear();g_queuedBytes=0;}g_cv.notify_all();if(g_thread.joinable())g_thread.join();std::lock_guard<std::mutex> lock(g_mutex);g_jobs.clear();g_readyBytes=0;g_stopping=false;
    }
}
