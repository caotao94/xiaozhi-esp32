#ifndef METAOS_IMAGE_H
#define METAOS_IMAGE_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace metaos {

enum class ImageError {
    kNone,
    kOpenFailed,
    kUnsupported,  // a format, or a variant (interlaced PNG, progressive JPEG), we cannot decode
    kTooLarge,
    kNoMemory,
    kCorrupt,
};

// A decoded image in PSRAM.
//   kBitmap: RGB565 pixels, `width` x `height`, scaled down to fit the requested box.
//   kGif:    the whole GIF file (plus a terminating pad) for LvglGif, which decodes frames as
//            it plays; `width` x `height` is the GIF's logical screen size.
struct DecodedImage {
    enum class Kind { kBitmap, kGif };

    Kind kind = Kind::kBitmap;
    int width = 0;
    int height = 0;
    uint8_t* data = nullptr;
    size_t size = 0;

    DecodedImage() = default;
    DecodedImage(const DecodedImage&) = delete;
    DecodedImage& operator=(const DecodedImage&) = delete;
    ~DecodedImage();
};

// Any task except the main task (it blocks on SD card I/O and decoding for up to a few
// seconds). Detects the format from the file contents. Bitmaps are never upscaled.
std::unique_ptr<DecodedImage> DecodeImage(const std::string& path, int max_width, int max_height,
                                          ImageError& error);

}  // namespace metaos

#endif  // METAOS_IMAGE_H
