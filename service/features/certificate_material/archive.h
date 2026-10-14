#pragma once

#include <zlib.h>

#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace service::certificate_material {

namespace detail {

struct zip_entry final {
    std::string filename_;
    std::string compressed_;
    std::uint32_t checksum_;
    std::uint32_t uncompressed_size_;
    std::uint32_t local_header_offset_;
};

template <typename value_type>
    requires std::is_unsigned_v<value_type>
inline void append_little_endian(std::string& output, value_type value) {
    for (std::size_t index = 0; index < sizeof(value_type); ++index) {
        output.push_back(static_cast<char>(value & static_cast<value_type>(0xff)));
        value >>= 8;
    }
}

inline std::uint32_t zip_size(std::size_t value, std::string_view field) {
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("certificate archive " + std::string(field) +
                                 " exceeds ZIP32 limits");
    }
    return static_cast<std::uint32_t>(value);
}

inline std::uint16_t zip_filename_size(std::size_t value) {
    if (value > std::numeric_limits<std::uint16_t>::max()) {
        throw std::runtime_error("certificate archive filename exceeds ZIP limits");
    }
    return static_cast<std::uint16_t>(value);
}

inline std::string deflate_for_zip(std::string_view input) {
    if (input.size() > std::numeric_limits<uInt>::max()) {
        throw std::runtime_error("certificate file exceeds deflate limits");
    }

    z_stream stream{};
    if (deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) !=
        Z_OK) {
        throw std::runtime_error("failed to initialize certificate archive compression");
    }
    struct stream_guard final {
        z_stream* stream_;
        ~stream_guard() {
            deflateEnd(stream_);
        }
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

inline zip_entry make_zip_entry(std::string filename, std::string_view content) {
    if (content.empty()) {
        throw std::runtime_error("certificate archive entry is empty");
    }
    const auto checksum = crc32_z(crc32(0L, Z_NULL, 0),
        reinterpret_cast<const Bytef*>(content.data()), content.size());
    return {
        .filename_ = std::move(filename),
        .compressed_ = deflate_for_zip(content),
        .checksum_ = static_cast<std::uint32_t>(checksum),
        .uncompressed_size_ = zip_size(content.size(), "entry"),
        .local_header_offset_ = 0,
    };
}

inline void append_local_header(std::string& archive, zip_entry& entry) {
    entry.local_header_offset_ = zip_size(archive.size(), "offset");
    append_little_endian<std::uint32_t>(archive, 0x04034b50);
    append_little_endian<std::uint16_t>(archive, 20);
    append_little_endian<std::uint16_t>(archive, 0x0800);
    append_little_endian<std::uint16_t>(archive, 8);
    append_little_endian<std::uint16_t>(archive, 0);
    append_little_endian<std::uint16_t>(archive, 0x0021);
    append_little_endian<std::uint32_t>(archive, entry.checksum_);
    append_little_endian<std::uint32_t>(archive, zip_size(entry.compressed_.size(), "entry"));
    append_little_endian<std::uint32_t>(archive, entry.uncompressed_size_);
    append_little_endian<std::uint16_t>(archive, zip_filename_size(entry.filename_.size()));
    append_little_endian<std::uint16_t>(archive, 0);
    archive.append(entry.filename_);
    archive.append(entry.compressed_);
}

inline void append_central_header(std::string& archive, const zip_entry& entry) {
    append_little_endian<std::uint32_t>(archive, 0x02014b50);
    append_little_endian<std::uint16_t>(archive, 20);
    append_little_endian<std::uint16_t>(archive, 20);
    append_little_endian<std::uint16_t>(archive, 0x0800);
    append_little_endian<std::uint16_t>(archive, 8);
    append_little_endian<std::uint16_t>(archive, 0);
    append_little_endian<std::uint16_t>(archive, 0x0021);
    append_little_endian<std::uint32_t>(archive, entry.checksum_);
    append_little_endian<std::uint32_t>(archive, zip_size(entry.compressed_.size(), "entry"));
    append_little_endian<std::uint32_t>(archive, entry.uncompressed_size_);
    append_little_endian<std::uint16_t>(archive, zip_filename_size(entry.filename_.size()));
    append_little_endian<std::uint16_t>(archive, 0);
    append_little_endian<std::uint16_t>(archive, 0);
    append_little_endian<std::uint16_t>(archive, 0);
    append_little_endian<std::uint16_t>(archive, 0);
    append_little_endian<std::uint32_t>(archive, 0);
    append_little_endian<std::uint32_t>(archive, entry.local_header_offset_);
    archive.append(entry.filename_);
}

}  // namespace detail

inline std::string build_archive(std::string_view filename, std::string_view certificate_chain,
    std::string_view private_key) {
    std::array entries{
        detail::make_zip_entry(std::string(filename) + ".crt", certificate_chain),
        detail::make_zip_entry(std::string(filename) + ".key", private_key),
    };
    std::string archive;
    archive.reserve(certificate_chain.size() + private_key.size() + 256);
    for (auto& entry : entries) {
        detail::append_local_header(archive, entry);
    }

    const auto central_offset = detail::zip_size(archive.size(), "central directory offset");
    for (const auto& entry : entries) {
        detail::append_central_header(archive, entry);
    }
    const auto central_size = detail::zip_size(archive.size() - central_offset, "central directory");
    detail::append_little_endian<std::uint32_t>(archive, 0x06054b50);
    detail::append_little_endian<std::uint16_t>(archive, 0);
    detail::append_little_endian<std::uint16_t>(archive, 0);
    detail::append_little_endian<std::uint16_t>(archive, static_cast<std::uint16_t>(entries.size()));
    detail::append_little_endian<std::uint16_t>(archive, static_cast<std::uint16_t>(entries.size()));
    detail::append_little_endian<std::uint32_t>(archive, central_size);
    detail::append_little_endian<std::uint32_t>(archive, central_offset);
    detail::append_little_endian<std::uint16_t>(archive, 0);
    return archive;
}

}  // namespace service::certificate_material
