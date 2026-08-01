#ifndef WE_SCENE_CORE_BINARY_READER_HPP
#define WE_SCENE_CORE_BINARY_READER_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace we::scene {

class BinaryReader final {
public:
    BinaryReader(std::span<const std::uint8_t> bytes, std::string source);

    [[nodiscard]] std::size_t position() const noexcept;
    [[nodiscard]] std::size_t remaining() const noexcept;
    [[nodiscard]] const std::string& source() const noexcept;

    void seek(std::size_t offset);
    void skip(std::size_t count);

    [[nodiscard]] std::span<const std::uint8_t> readExact(std::size_t count);
    [[nodiscard]] std::uint32_t readUInt32();
    [[nodiscard]] std::int32_t readInt32();
    [[nodiscard]] float readFloat32();
    [[nodiscard]] std::string readSizedString(std::size_t maximumLength);
    [[nodiscard]] std::string readNullTerminatedString(
        std::size_t maximumLength
    );

private:
    std::span<const std::uint8_t> bytes_;
    std::string source_;
    std::size_t position_ = 0;
};

[[nodiscard]] std::vector<std::uint8_t> readBinaryFile(
    const std::filesystem::path& path
);

}  // namespace we::scene

#endif
