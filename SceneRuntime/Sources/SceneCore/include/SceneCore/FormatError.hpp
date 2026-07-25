#ifndef WE_SCENE_CORE_FORMAT_ERROR_HPP
#define WE_SCENE_CORE_FORMAT_ERROR_HPP

#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>

namespace we::scene {

enum class FormatErrorCode {
    invalidArgument,
    ioFailure,
    unexpectedEndOfFile,
    invalidMagic,
    invalidValue,
    invalidOffset,
    duplicateEntry,
    unsafePath,
    assetNotFound,
    unsupportedFormat,
    decompressionFailed,
    malformedMetadata,
};

class FormatError final : public std::runtime_error {
public:
    static constexpr std::size_t noOffset =
        std::numeric_limits<std::size_t>::max();

    FormatError(
        FormatErrorCode code,
        std::string source,
        std::size_t offset,
        std::string message
    );

    [[nodiscard]] FormatErrorCode code() const noexcept;
    [[nodiscard]] const std::string& source() const noexcept;
    [[nodiscard]] std::size_t offset() const noexcept;

private:
    FormatErrorCode code_;
    std::string source_;
    std::size_t offset_;
};

}  // namespace we::scene

#endif
