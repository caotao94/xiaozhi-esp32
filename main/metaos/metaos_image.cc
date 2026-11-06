#include "metaos_image.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_rom_crc.h>
#include <miniz.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "jpeg_to_image.h"

#define TAG "MetaImage"

namespace metaos {

namespace {

constexpr size_t kGifTerminatorBytes = 16;  // Pad for gifdec safety

// Read up to `size` bytes from the file. Returns bytes read, or 0 on error.
size_t ReadChunk(FILE* f, void* buffer, size_t size) {
    size_t got = fread(buffer, 1, size, f);
    if (got < size && ferror(f)) {
        return 0;
    }
    return got;
}

// Detect format from file header
enum class Format { kJpeg, kPng, kGif, kBmp, kUnknown };

Format DetectFormat(const uint8_t* header, size_t len) {
    if (len < 8) {
        return Format::kUnknown;
    }
    // JPEG: FF D8 FF
    if (header[0] == 0xFF && header[1] == 0xD8 && header[2] == 0xFF) {
        return Format::kJpeg;
    }
    // PNG: 89 50 4E 47 0D 0A 1A 0A
    if (header[0] == 0x89 && header[1] == 0x50 && header[2] == 0x4E && header[3] == 0x47) {
        return Format::kPng;
    }
    // GIF: GIF87a or GIF89a
    if (memcmp(header, "GIF87a", 6) == 0 || memcmp(header, "GIF89a", 6) == 0) {
        return Format::kGif;
    }
    // BMP: 42 4D (BM)
    if (header[0] == 0x42 && header[1] == 0x4D) {
        return Format::kBmp;
    }
    return Format::kUnknown;
}

// JPEG: use existing jpeg_to_image_fit
std::unique_ptr<DecodedImage> DecodeJpeg(FILE* f, size_t file_size, int max_width, int max_height,
                                         ImageError& error) {
    if (file_size > 8 * 1024 * 1024) {
        ESP_LOGW(TAG, "JPEG too large: %zu bytes", file_size);
        error = ImageError::kTooLarge;
        return nullptr;
    }

    uint8_t* jpeg_data = static_cast<uint8_t*>(heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM));
    if (jpeg_data == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate %zu bytes for JPEG", file_size);
        error = ImageError::kNoMemory;
        return nullptr;
    }

    if (ReadChunk(f, jpeg_data, file_size) != file_size) {
        ESP_LOGE(TAG, "Failed to read JPEG data");
        heap_caps_free(jpeg_data);
        error = ImageError::kCorrupt;
        return nullptr;
    }

    uint8_t* rgb565 = nullptr;
    size_t rgb565_size = 0;
    size_t width = 0, height = 0, stride = 0;

    esp_err_t ret = jpeg_to_image_fit(jpeg_data, file_size, max_width, max_height, &rgb565,
                                      &rgb565_size, &width, &height, &stride);
    heap_caps_free(jpeg_data);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "jpeg_to_image_fit failed: %d", ret);
        error = ImageError::kCorrupt;
        return nullptr;
    }

    auto image = std::make_unique<DecodedImage>();
    image->kind = DecodedImage::Kind::kBitmap;
    image->width = width;
    image->height = height;
    image->data = rgb565;
    image->size = rgb565_size;
    error = ImageError::kNone;
    return image;
}

// PNG: stream decode with miniz
struct PngChunk {
    uint32_t length;
    uint32_t type;
};

bool ReadPngChunk(FILE* f, PngChunk& chunk) {
    uint8_t header[8];
    if (ReadChunk(f, header, 8) != 8) {
        return false;
    }
    chunk.length = (header[0] << 24) | (header[1] << 16) | (header[2] << 8) | header[3];
    chunk.type = (header[4] << 24) | (header[5] << 16) | (header[6] << 8) | header[7];
    return true;
}

std::unique_ptr<DecodedImage> DecodePng(FILE* f, int max_width, int max_height, ImageError& error) {
    // Read PNG header
    uint8_t sig[8];
    if (ReadChunk(f, sig, 8) != 8) {
        error = ImageError::kCorrupt;
        return nullptr;
    }

    // Read IHDR chunk
    PngChunk chunk;
    if (!ReadPngChunk(f, chunk) || chunk.type != 0x49484452) {  // 'IHDR'
        ESP_LOGE(TAG, "PNG: missing IHDR");
        error = ImageError::kCorrupt;
        return nullptr;
    }

    uint8_t ihdr[13];
    if (ReadChunk(f, ihdr, 13) != 13) {
        error = ImageError::kCorrupt;
        return nullptr;
    }
    fseek(f, 4, SEEK_CUR);  // Skip CRC

    uint32_t width = (ihdr[0] << 24) | (ihdr[1] << 16) | (ihdr[2] << 8) | ihdr[3];
    uint32_t height = (ihdr[4] << 24) | (ihdr[5] << 16) | (ihdr[6] << 8) | ihdr[7];
    uint8_t bit_depth = ihdr[8];
    uint8_t color_type = ihdr[9];
    uint8_t interlace = ihdr[12];

    ESP_LOGI(TAG, "PNG: %lux%lu, depth=%u, color=%u, interlace=%u", width, height, bit_depth,
             color_type, interlace);

    if (interlace != 0) {
        ESP_LOGW(TAG, "PNG: interlaced not supported");
        error = ImageError::kUnsupported;
        return nullptr;
    }

    if (width > 4096 || height > 4096) {
        ESP_LOGW(TAG, "PNG too large: %lux%lu", width, height);
        error = ImageError::kTooLarge;
        return nullptr;
    }

    // Collect IDAT chunks
    std::vector<uint8_t> compressed;
    compressed.reserve(64 * 1024);

    while (ReadPngChunk(f, chunk)) {
        if (chunk.type == 0x49454E44) {  // 'IEND'
            break;
        }
        if (chunk.type == 0x49444154) {  // 'IDAT'
            size_t start = compressed.size();
            compressed.resize(start + chunk.length);
            if (ReadChunk(f, compressed.data() + start, chunk.length) != chunk.length) {
                error = ImageError::kCorrupt;
                return nullptr;
            }
            fseek(f, 4, SEEK_CUR);  // Skip CRC
        } else {
            fseek(f, chunk.length + 4, SEEK_CUR);  // Skip data + CRC
        }
    }

    if (compressed.empty()) {
        ESP_LOGE(TAG, "PNG: no IDAT chunks");
        error = ImageError::kCorrupt;
        return nullptr;
    }

    // Decompress with miniz
    size_t bytes_per_pixel = (color_type == 2) ? 3 : (color_type == 6) ? 4 : 1;
    if (bit_depth == 16) {
        bytes_per_pixel *= 2;
    }
    size_t stride = width * bytes_per_pixel + 1;  // +1 for filter byte
    size_t raw_size = stride * height;

    uint8_t* raw = static_cast<uint8_t*>(heap_caps_malloc(raw_size, MALLOC_CAP_SPIRAM));
    if (raw == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate %zu bytes for PNG raw", raw_size);
        error = ImageError::kNoMemory;
        return nullptr;
    }

    // Inflate
    tinfl_decompressor inflator;
    tinfl_init(&inflator);
    size_t in_pos = 0, out_pos = 0;
    int status = TINFL_STATUS_NEEDS_MORE_INPUT;

    while (status == TINFL_STATUS_NEEDS_MORE_INPUT || status == TINFL_STATUS_HAS_MORE_OUTPUT) {
        size_t in_avail = compressed.size() - in_pos;
        size_t out_avail = raw_size - out_pos;
        size_t in_consumed = in_avail;
        size_t out_written = out_avail;

        status = tinfl_decompress(
            &inflator, compressed.data() + in_pos, &in_consumed, raw, raw + out_pos, &out_written,
            TINFL_FLAG_HAS_MORE_INPUT | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);

        in_pos += in_consumed;
        out_pos += out_written;

        if (status < 0) {
            ESP_LOGE(TAG, "PNG: inflate failed: %d", status);
            heap_caps_free(raw);
            error = ImageError::kCorrupt;
            return nullptr;
        }
    }

    if (out_pos < raw_size) {
        ESP_LOGW(TAG, "PNG: incomplete inflate: %zu/%zu", out_pos, raw_size);
    }

    // Unfilter scanlines
    for (uint32_t y = 0; y < height && y * stride < out_pos; y++) {
        uint8_t* line = raw + y * stride;
        uint8_t filter = line[0];
        uint8_t* prev = (y > 0) ? (line - stride + 1) : nullptr;

        for (size_t x = 1; x < stride; x++) {
            uint8_t left = (x > bytes_per_pixel) ? line[x - bytes_per_pixel] : 0;
            uint8_t up = prev ? prev[x - 1] : 0;
            uint8_t up_left = (prev && x > bytes_per_pixel) ? prev[x - 1 - bytes_per_pixel] : 0;

            if (filter == 1) {  // Sub
                line[x] = line[x] + left;
            } else if (filter == 2) {  // Up
                line[x] = line[x] + up;
            } else if (filter == 3) {  // Average
                line[x] = line[x] + ((left + up) / 2);
            } else if (filter == 4) {  // Paeth
                int p = left + up - up_left;
                int pa = abs(p - left), pb = abs(p - up), pc = abs(p - up_left);
                uint8_t predictor = (pa <= pb && pa <= pc) ? left : (pb <= pc) ? up : up_left;
                line[x] = line[x] + predictor;
            }
        }
    }

    // Convert to RGB565 with scaling
    int scale = 1;
    while (width / scale > (uint32_t)max_width || height / scale > (uint32_t)max_height) {
        scale++;
    }

    uint32_t out_width = width / scale;
    uint32_t out_height = height / scale;
    size_t out_size = out_width * out_height * 2;

    uint8_t* rgb565 = static_cast<uint8_t*>(heap_caps_malloc(out_size, MALLOC_CAP_SPIRAM));
    if (rgb565 == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate %zu bytes for RGB565", out_size);
        heap_caps_free(raw);
        error = ImageError::kNoMemory;
        return nullptr;
    }

    for (uint32_t y = 0; y < out_height; y++) {
        for (uint32_t x = 0; x < out_width; x++) {
            uint32_t src_y = (y * scale) < height ? (y * scale) : (height - 1);
            uint32_t src_x = (x * scale) < width ? (x * scale) : (width - 1);
            uint8_t* pixel = raw + src_y * stride + 1 + src_x * bytes_per_pixel;

            uint8_t r = 0, g = 0, b = 0;
            if (color_type == 0) {  // Grayscale
                r = g = b = pixel[0];
            } else if (color_type == 2) {  // RGB
                r = pixel[0];
                g = pixel[1];
                b = pixel[2];
            } else if (color_type == 6) {  // RGBA
                r = pixel[0];
                g = pixel[1];
                b = pixel[2];
            }

            uint16_t rgb565_pixel = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
            rgb565[(y * out_width + x) * 2] = rgb565_pixel & 0xFF;
            rgb565[(y * out_width + x) * 2 + 1] = rgb565_pixel >> 8;
        }
    }

    heap_caps_free(raw);

    auto image = std::make_unique<DecodedImage>();
    image->kind = DecodedImage::Kind::kBitmap;
    image->width = out_width;
    image->height = out_height;
    image->data = rgb565;
    image->size = out_size;
    error = ImageError::kNone;
    ESP_LOGI(TAG, "PNG decoded: %lux%lu -> %dx%d", width, height, out_width, out_height);
    return image;
}

// GIF: load entire file for LvglGif
std::unique_ptr<DecodedImage> DecodeGif(FILE* f, size_t file_size, ImageError& error) {
    if (file_size > 4 * 1024 * 1024) {
        ESP_LOGW(TAG, "GIF too large: %zu bytes", file_size);
        error = ImageError::kTooLarge;
        return nullptr;
    }

    size_t alloc_size = file_size + kGifTerminatorBytes;
    uint8_t* gif_data = static_cast<uint8_t*>(heap_caps_malloc(alloc_size, MALLOC_CAP_SPIRAM));
    if (gif_data == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate %zu bytes for GIF", alloc_size);
        error = ImageError::kNoMemory;
        return nullptr;
    }

    fseek(f, 0, SEEK_SET);
    if (ReadChunk(f, gif_data, file_size) != file_size) {
        ESP_LOGE(TAG, "Failed to read GIF data");
        heap_caps_free(gif_data);
        error = ImageError::kCorrupt;
        return nullptr;
    }

    // Zero terminator bytes for gifdec safety
    memset(gif_data + file_size, 0, kGifTerminatorBytes);

    // Parse GIF header to get dimensions
    if (file_size < 10) {
        heap_caps_free(gif_data);
        error = ImageError::kCorrupt;
        return nullptr;
    }

    uint16_t width = gif_data[6] | (gif_data[7] << 8);
    uint16_t height = gif_data[8] | (gif_data[9] << 8);

    auto image = std::make_unique<DecodedImage>();
    image->kind = DecodedImage::Kind::kGif;
    image->width = width;
    image->height = height;
    image->data = gif_data;
    image->size = alloc_size;
    error = ImageError::kNone;
    ESP_LOGI(TAG, "GIF loaded: %dx%d, %zu bytes", width, height, file_size);
    return image;
}

// BMP: stream decode, 24/32bpp uncompressed only
std::unique_ptr<DecodedImage> DecodeBmp(FILE* f, int max_width, int max_height, ImageError& error) {
    uint8_t header[54];
    if (ReadChunk(f, header, 54) != 54) {
        error = ImageError::kCorrupt;
        return nullptr;
    }

    uint32_t offset = header[10] | (header[11] << 8) | (header[12] << 16) | (header[13] << 24);
    uint32_t width = header[18] | (header[19] << 8) | (header[20] << 16) | (header[21] << 24);
    int32_t height_signed =
        header[22] | (header[23] << 8) | (header[24] << 16) | (header[25] << 24);
    uint32_t height = abs(height_signed);
    bool top_down = height_signed < 0;
    uint16_t bpp = header[28] | (header[29] << 8);
    uint32_t compression = header[30] | (header[31] << 8) | (header[32] << 16) | (header[33] << 24);

    ESP_LOGI(TAG, "BMP: %lux%lu, %ubpp, compression=%lu", width, height, bpp, compression);

    if (compression != 0 || (bpp != 24 && bpp != 32)) {
        ESP_LOGW(TAG, "BMP: unsupported format (compression or bpp)");
        error = ImageError::kUnsupported;
        return nullptr;
    }

    if (width > 4096 || height > 4096) {
        ESP_LOGW(TAG, "BMP too large: %lux%lu", width, height);
        error = ImageError::kTooLarge;
        return nullptr;
    }

    // Calculate scaling
    int scale = 1;
    while (width / scale > (uint32_t)max_width || height / scale > (uint32_t)max_height) {
        scale++;
    }

    uint32_t out_width = width / scale;
    uint32_t out_height = height / scale;
    size_t out_size = out_width * out_height * 2;

    uint8_t* rgb565 = static_cast<uint8_t*>(heap_caps_malloc(out_size, MALLOC_CAP_SPIRAM));
    if (rgb565 == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate %zu bytes for RGB565", out_size);
        error = ImageError::kNoMemory;
        return nullptr;
    }

    // BMP rows are padded to 4-byte boundary
    size_t row_bytes = ((width * bpp + 31) / 32) * 4;
    uint8_t* row = static_cast<uint8_t*>(malloc(row_bytes));
    if (row == nullptr) {
        heap_caps_free(rgb565);
        error = ImageError::kNoMemory;
        return nullptr;
    }

    fseek(f, offset, SEEK_SET);

    for (uint32_t y = 0; y < out_height; y++) {
        uint32_t src_y = (y * scale) < height ? (y * scale) : (height - 1);
        uint32_t file_row = top_down ? src_y : (height - 1 - src_y);

        fseek(f, offset + file_row * row_bytes, SEEK_SET);
        if (ReadChunk(f, row, row_bytes) != row_bytes) {
            free(row);
            heap_caps_free(rgb565);
            error = ImageError::kCorrupt;
            return nullptr;
        }

        for (uint32_t x = 0; x < out_width; x++) {
            uint32_t src_x = (x * scale) < width ? (x * scale) : (width - 1);
            size_t pixel_offset = src_x * (bpp / 8);

            uint8_t b = row[pixel_offset];
            uint8_t g = row[pixel_offset + 1];
            uint8_t r = row[pixel_offset + 2];

            uint16_t rgb565_pixel = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
            rgb565[(y * out_width + x) * 2] = rgb565_pixel & 0xFF;
            rgb565[(y * out_width + x) * 2 + 1] = rgb565_pixel >> 8;
        }
    }

    free(row);

    auto image = std::make_unique<DecodedImage>();
    image->kind = DecodedImage::Kind::kBitmap;
    image->width = out_width;
    image->height = out_height;
    image->data = rgb565;
    image->size = out_size;
    error = ImageError::kNone;
    ESP_LOGI(TAG, "BMP decoded: %lux%lu -> %dx%d", width, height, out_width, out_height);
    return image;
}

}  // namespace

DecodedImage::~DecodedImage() {
    if (data != nullptr) {
        heap_caps_free(data);
    }
}

std::unique_ptr<DecodedImage> DecodeImage(const std::string& path, int max_width, int max_height,
                                          ImageError& error) {
    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) {
        ESP_LOGE(TAG, "Failed to open: %s", path.c_str());
        error = ImageError::kOpenFailed;
        return nullptr;
    }

    // Get file size
    fseek(f, 0, SEEK_END);
    long file_size_long = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size_long < 0) {
        fclose(f);
        error = ImageError::kOpenFailed;
        return nullptr;
    }
    size_t file_size = file_size_long;

    // Detect format
    uint8_t header[16];
    size_t header_size = ReadChunk(f, header, sizeof(header));
    if (header_size < 8) {
        fclose(f);
        error = ImageError::kCorrupt;
        return nullptr;
    }
    fseek(f, 0, SEEK_SET);

    Format format = DetectFormat(header, header_size);
    ESP_LOGI(TAG, "Decoding %s: format=%d, size=%zu", path.c_str(), (int)format, file_size);

    std::unique_ptr<DecodedImage> result;
    switch (format) {
        case Format::kJpeg:
            result = DecodeJpeg(f, file_size, max_width, max_height, error);
            break;
        case Format::kPng:
            result = DecodePng(f, max_width, max_height, error);
            break;
        case Format::kGif:
            result = DecodeGif(f, file_size, error);
            break;
        case Format::kBmp:
            result = DecodeBmp(f, max_width, max_height, error);
            break;
        default:
            ESP_LOGW(TAG, "Unknown format");
            error = ImageError::kUnsupported;
            break;
    }

    fclose(f);
    return result;
}

}  // namespace metaos
