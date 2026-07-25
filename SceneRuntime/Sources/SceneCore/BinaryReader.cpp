#include <SceneCore/BinaryReader.hpp>

#include <SceneCore/FormatError.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <utility>

namespace we::scene {

BinaryReader::BinaryReader(
    std::span<const std::uint8_t> bytes,
    std::string source
)
    : bytes_(bytes), source_(std::move(source)) {}

std::size_t BinaryReader::position() const noexcept {
    return position_;
}

std::size_t BinaryReader::remaining() const noexcept {
    return bytes_.size() - position_;
}

const std::string& BinaryReader::source() const noexcept {
    return source_;
}

void BinaryReader::seek(std::size_t offset) {
    if (offset > bytes_.size()) {
        throw FormatError(
            FormatErrorCode::unexpectedEndOfFile,
            source_,
            position_,
            "Seek target is beyond the end of the input"
        );
    }
    position_ = offset;
}

void BinaryReader::skip(std::size_t count) {
    static_cast<void>(readExact(count));
}

std::span<const std::uint8_t> BinaryReader::readExact(std::size_t count) {
    if (count > remaining()) {
        std::ostringstream message;
        message << "Unexpected end of input: requested " << count
                << " bytes but only " << remaining() << " remain";
        throw FormatError(
            FormatErrorCode::unexpectedEndOfFile,
            source_,
            position_,
            message.str()
        );
    }

    const auto result = bytes_.subspan(position_, count);
    position_ += count;
    return result;
}

std::uint32_t BinaryReader::readUInt32() {
    const auto value = readExact(sizeof(std::uint32_t));
    return static_cast<std::uint32_t>(value[0]) |
        (static_cast<std::uint32_t>(value[1]) << 8U) |
        (static_cast<std::uint32_t>(value[2]) << 16U) |
        (static_cast<std::uint32_t>(value[3]) << 24U);
}

std::int32_t BinaryReader::readInt32() {
    return static_cast<std::int32_t>(readUInt32());
}

float BinaryReader::readFloat32() {
    const std::uint32_t bits = readUInt32();
    float value = 0.0F;
    static_assert(sizeof(value) == sizeof(bits));
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

std::string BinaryReader::readSizedString(std::size_t maximumLength) {
    const std::size_t length = readUInt32();
    if (length > maximumLength) {
        std::ostringstream message;
        message << "Length-prefixed string exceeds the limit of "
                << maximumLength << " bytes (declared " << length << ')';
        throw FormatError(
            FormatErrorCode::invalidValue,
            source_,
            position_ - sizeof(std::uint32_t),
            message.str()
        );
    }
    const auto value = readExact(length);
    return {
        reinterpret_cast<const char*>(value.data()),
        value.size(),
    };
}

std::string BinaryReader::readNullTerminatedString(
    std::size_t maximumLength
) {
    const std::size_t start = position_;
    const std::size_t limit = std::min(maximumLength, remaining());
    for (std::size_t index = 0; index < limit; ++index) {
        if (bytes_[position_ + index] == 0) {
            const auto value = readExact(index);
            skip(1);
            return {
                reinterpret_cast<const char*>(value.data()),
                value.size(),
            };
        }
    }

    throw FormatError(
        FormatErrorCode::invalidValue,
        source_,
        start,
        remaining() < maximumLength
            ? "Unterminated string reaches the end of the input"
            : "Null-terminated string exceeds its maximum length"
    );
}

std::vector<std::uint8_t> readBinaryFile(
    const std::filesystem::path& path
) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input.is_open()) {
        throw FormatError(
            FormatErrorCode::ioFailure,
            path.string(),
            FormatError::noOffset,
            "Unable to open binary file"
        );
    }

    const std::streampos end = input.tellg();
    if (end < 0) {
        throw FormatError(
            FormatErrorCode::ioFailure,
            path.string(),
            FormatError::noOffset,
            "Unable to determine binary file size"
        );
    }
    if (static_cast<std::uintmax_t>(end) >
        std::numeric_limits<std::size_t>::max()) {
        throw FormatError(
            FormatErrorCode::invalidValue,
            path.string(),
            FormatError::noOffset,
            "Binary file is too large for this process"
        );
    }

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    input.seekg(0, std::ios::beg);
    if (!bytes.empty()) {
        input.read(
            reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size())
        );
    }
    if (!input || input.gcount() != static_cast<std::streamsize>(bytes.size())) {
        throw FormatError(
            FormatErrorCode::ioFailure,
            path.string(),
            FormatError::noOffset,
            "Unable to read the complete binary file"
        );
    }
    return bytes;
}

}  // namespace we::scene
