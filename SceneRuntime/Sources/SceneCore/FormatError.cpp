#include <SceneCore/FormatError.hpp>

#include <sstream>
#include <utility>

namespace we::scene {
namespace {

std::string formatMessage(
    const std::string& source,
    std::size_t offset,
    const std::string& message
) {
    std::ostringstream output;
    output << message;
    if (!source.empty()) {
        output << " [source: " << source;
        if (offset != FormatError::noOffset) {
            output << ", offset: " << offset;
        }
        output << ']';
    }
    return output.str();
}

}  // namespace

FormatError::FormatError(
    FormatErrorCode code,
    std::string source,
    std::size_t offset,
    std::string message
)
    : std::runtime_error(formatMessage(source, offset, message)),
      code_(code),
      source_(std::move(source)),
      offset_(offset) {}

FormatErrorCode FormatError::code() const noexcept {
    return code_;
}

const std::string& FormatError::source() const noexcept {
    return source_;
}

std::size_t FormatError::offset() const noexcept {
    return offset_;
}

}  // namespace we::scene
