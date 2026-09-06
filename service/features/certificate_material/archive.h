#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <zlib.h>

namespace service::certificate_material {

namespace detail {

struct ZipEntry final {
    std::string filename;
    std::string compressed;
    std::uint32_t checksum;
    std::uint32_t uncompressedSize;
    std::uint32_t localHeaderOffset;
};

template <typename Value>
    requires std::is_unsigned_v<Value>
inline void appendLittleEndian(std::string& output, Value value) {
    for (std::size_t index = 0; index < sizeof(Value); ++index) {
        output.push_back(static_cast<char>(value & static_cast<Value>(0xff)));
        value >>= 8;
    }
}

inline std::uint32_t zipSize(std::size_t value, std::string_view field) {
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("certificate archive " + std::string(field) +
                                 " exceeds ZIP32 limits");
    }
    return static_cast<std::uint32_t>(value);
}

inline std::uint16_t zipFilenameSize(std::size_t value) {
    if (value > std::numeric_limits<std::uint16_t>::max()) {
        throw std::runtime_error("certificate archive filename exceeds ZIP limits");
    }
    return static_cast<std::uint16_t>(value);
}

inline std::string deflateForZip(std::string_view input) {
    if (input.size() > std::numeric_limits<uInt>::max()) {
        throw std::runtime_error("certificate file exceeds deflate limits");
    }

    z_stream stream{};
    if (deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) !=
        Z_OK) {
        throw std::runtime_error("failed to initialize certificate archive compression");
    }
    struct StreamGuard final {
        z_stream* stream;
        ~StreamGuard() { deflateEnd(stream); }
    } guard{&stream};

    const auto bound = deflateBound(&stream, static_cast<uLong>(input.size()));
    if (bound > std::numeric_limits<uInt>::max()) {
        throw std::runtime_error("compressed certificate file exceeds deflate limits");
    }
    std::string output(static_cast<std::size_t>(bound), '\0');
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());
    stream.next_out = reinterpret_cast<Bytef*>(output.data());
    stream.avail_out = static_cast<uInt>(output.size());
    if (deflate(&stream, Z_FINISH) != Z_STREAM_END) {
        throw std::runtime_error("failed to compress certificate archive entry");
    }
    output.resize(static_cast<std::size_t>(stream.total_out));
    return output;
}

inline ZipEntry makeZipEntry(std::string filename, std::string_view content) {
    if (content.empty()) {
        throw std::runtime_error("certificate archive entry is empty");
    }
    const auto checksum = crc32_z(crc32(0L, Z_NULL, 0),
                                  reinterpret_cast<const Bytef*>(content.data()), content.size());
    return {
        .filename = std::move(filename),
        .compressed = deflateForZip(content),
        .checksum = static_cast<std::uint32_t>(checksum),
        .uncompressedSize = zipSize(content.size(), "entry"),
        .localHeaderOffset = 0,
    };
}

inline void appendLocalHeader(std::string& archive, ZipEntry& entry) {
    entry.localHeaderOffset = zipSize(archive.size(), "offset");
    appendLittleEndian<std::uint32_t>(archive, 0x04034b50);
    appendLittleEndian<std::uint16_t>(archive, 20);
    appendLittleEndian<std::uint16_t>(archive, 0x0800);
    appendLittleEndian<std::uint16_t>(archive, 8);
    appendLittleEndian<std::uint16_t>(archive, 0);
    appendLittleEndian<std::uint16_t>(archive, 0x0021);
    appendLittleEndian<std::uint32_t>(archive, entry.checksum);
    appendLittleEndian<std::uint32_t>(archive, zipSize(entry.compressed.size(), "entry"));
    appendLittleEndian<std::uint32_t>(archive, entry.uncompressedSize);
    appendLittleEndian<std::uint16_t>(archive, zipFilenameSize(entry.filename.size()));
    appendLittleEndian<std::uint16_t>(archive, 0);
    archive.append(entry.filename);
    archive.append(entry.compressed);
}

inline void appendCentralHeader(std::string& archive, const ZipEntry& entry) {
    appendLittleEndian<std::uint32_t>(archive, 0x02014b50);
    appendLittleEndian<std::uint16_t>(archive, 20);
    appendLittleEndian<std::uint16_t>(archive, 20);
    appendLittleEndian<std::uint16_t>(archive, 0x0800);
    appendLittleEndian<std::uint16_t>(archive, 8);
    appendLittleEndian<std::uint16_t>(archive, 0);
    appendLittleEndian<std::uint16_t>(archive, 0x0021);
    appendLittleEndian<std::uint32_t>(archive, entry.checksum);
    appendLittleEndian<std::uint32_t>(archive, zipSize(entry.compressed.size(), "entry"));
    appendLittleEndian<std::uint32_t>(archive, entry.uncompressedSize);
    appendLittleEndian<std::uint16_t>(archive, zipFilenameSize(entry.filename.size()));
    appendLittleEndian<std::uint16_t>(archive, 0);
    appendLittleEndian<std::uint16_t>(archive, 0);
    appendLittleEndian<std::uint16_t>(archive, 0);
    appendLittleEndian<std::uint16_t>(archive, 0);
    appendLittleEndian<std::uint32_t>(archive, 0);
    appendLittleEndian<std::uint32_t>(archive, entry.localHeaderOffset);
    archive.append(entry.filename);
}

} // namespace detail

inline std::string buildArchive(std::string_view filename, std::string_view certificateChain,
                                std::string_view privateKey) {
    std::array entries{
        detail::makeZipEntry(std::string(filename) + ".crt", certificateChain),
        detail::makeZipEntry(std::string(filename) + ".key", privateKey),
    };
    std::string archive;
    archive.reserve(certificateChain.size() + privateKey.size() + 256);
    for (auto& entry : entries) {
        detail::appendLocalHeader(archive, entry);
    }

    const auto centralOffset = detail::zipSize(archive.size(), "central directory offset");
    for (const auto& entry : entries) {
        detail::appendCentralHeader(archive, entry);
    }
    const auto centralSize = detail::zipSize(archive.size() - centralOffset, "central directory");
    detail::appendLittleEndian<std::uint32_t>(archive, 0x06054b50);
    detail::appendLittleEndian<std::uint16_t>(archive, 0);
    detail::appendLittleEndian<std::uint16_t>(archive, 0);
    detail::appendLittleEndian<std::uint16_t>(archive, static_cast<std::uint16_t>(entries.size()));
    detail::appendLittleEndian<std::uint16_t>(archive, static_cast<std::uint16_t>(entries.size()));
    detail::appendLittleEndian<std::uint32_t>(archive, centralSize);
    detail::appendLittleEndian<std::uint32_t>(archive, centralOffset);
    detail::appendLittleEndian<std::uint16_t>(archive, 0);
    return archive;
}

} // namespace service::certificate_material
