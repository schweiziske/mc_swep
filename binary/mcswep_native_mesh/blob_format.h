// blob_format.h —— 模块跨界数据契约 v0 的代码化身(唯一权威)
//
// 对应 Lua 侧: _tools/mc_native_transfer_bench.lua 头部注释
// 规则:
//   1. 本文件任何布局/版本改动必须同步通知合作者并升 version 常量;
//   2. 所有跨界输入按敌意数据处理: 只能经 BlobReader / Validate* 读取,
//      不允许未经长度校验的 reinterpret_cast;
//   3. 全部小端(LE)。仅支持 Windows x86/x64(天然 LE), 其他平台需另行处理。
//
// 依赖: C++17, 仅标准库。

#pragma once

#include <cstdint>
#include <cstring>
#include <cstddef>
#include <vector>
#include <type_traits>

namespace mcmesh {

    // ---------------------------------------------------------------- 常量
    constexpr int      ABI_VERSION = 0;      // mcmesh.AbiVersion() 返回值
    constexpr uint16_t VERTEX_BLOB_VERSION = 1;
    constexpr char     VERTEX_BLOB_MAGIC[4] = { 'M', 'C', 'V', 'B' };

    constexpr uint16_t VERTEX_FLAG_HAS_UV1 = 1 << 0;

    // 世界几何(握手时仍会和 Lua 侧核对, 这里是编译期默认值)
    constexpr int CHUNK_SIZE_XY = 16;
    constexpr int CHUNK_SIZE_Z = 32;
    constexpr int CHUNK_CELLS = CHUNK_SIZE_XY * CHUNK_SIZE_XY * CHUNK_SIZE_Z; // 8192

    // 顶点数上限: 与 Lua 侧 VERT_LIMIT (cl_mesh_constants.lua:60) 同源。
    // 校验用途——恶意 header 声称的 count 超过它直接拒绝。
    constexpr uint32_t MAX_VERTEX_COUNT = 64998;

    // ---------------------------------------------------------------- 布局
#pragma pack(push, 1)

// chunk blob: CHUNK_CELLS 个 ChunkCell 密集排列, 格序 li = lx + ly*16 + lz*256
    struct ChunkCell {
        uint32_t stateId;    // registry-local state id, 0 = 空气
    };

    // 顶点 blob: VertexBlobHeader 后跟 vertexCount 个 Vertex44
    struct VertexBlobHeader {
        char     magic[4];      // "MCVB"
        uint16_t version;       // VERTEX_BLOB_VERSION
        uint16_t flags;         // VERTEX_FLAG_*
        uint32_t vertexCount;
    };

    struct Vertex44 {
        float   pos[3];         // Source 世界坐标
        float   normal[3];
        float   u, v;           // atlas UV
        uint8_t rgba[4];        // GPU payload: R=255, G=faceIndex, B=0, A=alpha
        float   u1, v1;         // uv2 (lightmap), flags 无 HAS_UV1 时忽略
    };

#pragma pack(pop)

    static_assert(sizeof(ChunkCell) == 4, "ChunkCell must be 4 bytes");
    static_assert(sizeof(VertexBlobHeader) == 12, "VertexBlobHeader must be 12 bytes");
    static_assert(sizeof(Vertex44) == 44, "Vertex44 must be 44 bytes");
    static_assert(offsetof(Vertex44, u) == 24, "Vertex44 uv offset");
    static_assert(offsetof(Vertex44, rgba) == 32, "Vertex44 color offset");
    static_assert(offsetof(Vertex44, u1) == 36, "Vertex44 uv1 offset");

    constexpr size_t CHUNK_BLOB_BYTES = CHUNK_CELLS * sizeof(uint32_t); // stateId:u32 LE

    // ---------------------------------------------------------------- 写
    // 追加式字节缓冲。热路径用 WriteArray 整块进, 零散字段才用 Write<T>。
    class BlobWriter {
    public:
        void Reserve(size_t bytes) { buf_.reserve(bytes); }
        void Clear() { buf_.clear(); }

        template <typename T>
        void Write(const T& value) {
            static_assert(std::is_trivially_copyable_v<T>, "POD only");
            WriteBytes(&value, sizeof(T));
        }

        template <typename T>
        void WriteArray(const T* items, size_t count) {
            static_assert(std::is_trivially_copyable_v<T>, "POD only");
            WriteBytes(items, count * sizeof(T));
        }

        void WriteBytes(const void* data, size_t bytes) {
            const auto* p = static_cast<const uint8_t*>(data);
            buf_.insert(buf_.end(), p, p + bytes);
        }

        const char* Data() const { return reinterpret_cast<const char*>(buf_.data()); }
        size_t      Size() const { return buf_.size(); }

    private:
        std::vector<uint8_t> buf_;
    };

    // 便捷函数: 顶点数组 -> 完整顶点 blob(header + 顶点)
    inline void BuildVertexBlob(BlobWriter& out, const Vertex44* verts, uint32_t count, uint16_t flags) {
        VertexBlobHeader header{};
        std::memcpy(header.magic, VERTEX_BLOB_MAGIC, 4);
        header.version = VERTEX_BLOB_VERSION;
        header.flags = flags;
        header.vertexCount = count;
        out.Reserve(out.Size() + sizeof(header) + count * sizeof(Vertex44));
        out.Write(header);
        out.WriteArray(verts, count);
    }

    // ---------------------------------------------------------------- 读
    // 非拥有视图: 直接架在 LUA->GetString 返回的 (ptr, len) 上, 生命周期
    // 仅限当前调用内; 需要留存的数据必须 memcpy 走。
    // 所有读取带边界检查, 失败返回 false 且不移动游标。这是 fuzz 的目标类。
    class BlobReader {
    public:
        BlobReader(const void* data, size_t len)
            : data_(static_cast<const uint8_t*>(data)), len_(data ? len : 0) {}

        template <typename T>
        bool Read(T& out) {
            static_assert(std::is_trivially_copyable_v<T>, "POD only");
            return ReadBytes(&out, sizeof(T));
        }

        bool ReadBytes(void* out, size_t bytes) {
            if (!CanRead(bytes)) return false;
            std::memcpy(out, data_ + pos_, bytes);
            pos_ += bytes;
            return true;
        }

        // 取一段只读视图(不拷贝), 供整块 memcpy 到自有存储
        bool ReadView(const uint8_t*& outPtr, size_t bytes) {
            if (!CanRead(bytes)) return false;
            outPtr = data_ + pos_;
            pos_ += bytes;
            return true;
        }

        bool   CanRead(size_t bytes) const { return data_ != nullptr && len_ - pos_ >= bytes; }
        size_t Remaining() const { return len_ - pos_; }
        bool   AtEnd() const { return pos_ == len_; }

    private:
        const uint8_t* data_;
        size_t         len_;
        size_t         pos_ = 0;
    };

    // ---------------------------------------------------------------- 校验入口
    // ApplyChunk 用: 校验并整块拷入调用方的 cells (须容纳 CHUNK_CELLS 个)。
    // 长度必须严格等于 CHUNK_BLOB_BYTES。
    inline bool ParseChunkBlob(const void* data, size_t len, ChunkCell* cellsOut) {
        if (data == nullptr || cellsOut == nullptr) return false;
        if (len != CHUNK_BLOB_BYTES) return false;
        std::memcpy(cellsOut, data, CHUNK_BLOB_BYTES);
        return true;
    }

    // 顶点 blob 用(CLI 壳 / 测试 / 未来反向通道):
    // 校验 header 与总长, 输出 header 和指向顶点段的只读视图。
    // 注意 count 先查上限再参与乘法, 防溢出。
    inline bool ParseVertexBlob(const void* data, size_t len,
        VertexBlobHeader& headerOut, const Vertex44*& vertsOut) {
        BlobReader reader(data, len);
        if (!reader.Read(headerOut)) return false;
        if (std::memcmp(headerOut.magic, VERTEX_BLOB_MAGIC, 4) != 0) return false;
        if (headerOut.version != VERTEX_BLOB_VERSION) return false;
        if (headerOut.vertexCount > MAX_VERTEX_COUNT) return false;
        if (headerOut.vertexCount % 3 != 0) return false;   // 非索引三角形列表
        const size_t vertexBytes = size_t(headerOut.vertexCount) * sizeof(Vertex44);
        const uint8_t* view = nullptr;
        if (!reader.ReadView(view, vertexBytes)) return false;
        if (!reader.AtEnd()) return false;                  // 尾部多余字节也拒绝
        vertsOut = reinterpret_cast<const Vertex44*>(view);
        return true;
    }

} // namespace mcmesh
