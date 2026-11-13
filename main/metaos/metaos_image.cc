#include "metaos_image.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_rom_crc.h>
#include <miniz.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

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

// 持有一块 heap_caps 分配的内存，离开作用域自动释放。异常被禁用，DecodePng 的
// 流式解码有多条错误返回路径，用 RAII 保证 compressed/环形缓冲/行缓冲/输出位图
// 在任何路径上都不泄漏。heap_caps_free(nullptr) 是安全的空操作。
class PngScratch {
public:
    PngScratch() = default;
    ~PngScratch() { heap_caps_free(ptr_); }
    PngScratch(const PngScratch&) = delete;
    PngScratch& operator=(const PngScratch&) = delete;

    uint8_t* Alloc(size_t size, uint32_t caps) {
        ptr_ = static_cast<uint8_t*>(heap_caps_malloc(size, caps));
        return ptr_;
    }
    // 接管一块已分配的内存（如 IDAT 收集阶段分配的 compressed）。
    void Adopt(uint8_t* p) { ptr_ = p; }
    uint8_t* get() const { return ptr_; }
    // 交出所有权（成功时把输出位图转交给 DecodedImage）。
    uint8_t* Release() {
        uint8_t* p = ptr_;
        ptr_ = nullptr;
        return p;
    }

private:
    uint8_t* ptr_ = nullptr;
};

std::unique_ptr<DecodedImage> DecodePng(FILE* f, int max_width, int max_height, ImageError& error) {
    // Read PNG header
    uint8_t sig[8];
    if (ReadChunk(f, sig, 8) != 8) {
        error = ImageError::kCorrupt;
        return nullptr;
    }

    // Read IHDR chunk
    PngChunk chunk;
    if (!ReadPngChunk(f, chunk) || chunk.type != 0x49484452 || chunk.length != 13) {  // 'IHDR'
        ESP_LOGE(TAG, "PNG: missing or bad IHDR");
        error = ImageError::kCorrupt;
        return nullptr;
    }

    uint8_t ihdr[13];
    if (ReadChunk(f, ihdr, 13) != 13) {
        error = ImageError::kCorrupt;
        return nullptr;
    }
    if (fseek(f, 4, SEEK_CUR) != 0) {  // Skip CRC
        error = ImageError::kCorrupt;
        return nullptr;
    }

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

    if (width == 0 || height == 0) {
        ESP_LOGW(TAG, "PNG: zero dimension");
        error = ImageError::kCorrupt;
        return nullptr;
    }

    // 流式解码后内存不再随宽*高增长，尺寸上限放宽到 8192。
    // 行缓冲 stride 最大 8192*8+1≈64KB，仍可接受；IDAT 压缩上限保持 8MB。
    if (width > 8192 || height > 8192) {
        ESP_LOGW(TAG, "PNG too large: %lux%lu", width, height);
        error = ImageError::kTooLarge;
        return nullptr;
    }

    // Supported: grayscale(0), RGB(2), grayscale+alpha(4), RGBA(6) at 8/16-bit.
    // Palette(3) would need PLTE parsing; sub-byte depths would need unpacking.
    if ((color_type != 0 && color_type != 2 && color_type != 4 && color_type != 6) ||
        (bit_depth != 8 && bit_depth != 16)) {
        ESP_LOGW(TAG, "PNG: unsupported color/depth: %u/%u", color_type, bit_depth);
        error = ImageError::kUnsupported;
        return nullptr;
    }

    // Collect IDAT chunks in two passes: sum their sizes, then read into one
    // PSRAM allocation. The previous std::vector grew via operator new, which
    // abort()s on allocation failure because C++ exceptions are disabled; a
    // 1024x1024 PNG crashed the device this way. heap_caps_malloc lets us
    // return kNoMemory instead. The 8MB cap bounds the compressed IDAT buffer
    // held in PSRAM during streaming decode; larger inputs fail fast with kTooLarge.
    constexpr size_t kMaxCompressedBytes = 8 * 1024 * 1024;
    long idat_start = ftell(f);
    if (idat_start < 0) {
        error = ImageError::kCorrupt;
        return nullptr;
    }

    size_t compressed_size = 0;
    while (ReadPngChunk(f, chunk)) {
        if (chunk.type == 0x49454E44) {  // 'IEND'
            break;
        }
        if (chunk.length > kMaxCompressedBytes) {
            ESP_LOGW(TAG, "PNG: chunk too large: %lu", chunk.length);
            error = ImageError::kTooLarge;
            return nullptr;
        }
        if (chunk.type == 0x49444154) {  // 'IDAT'
            if (chunk.length > kMaxCompressedBytes - compressed_size) {
                ESP_LOGW(TAG, "PNG: IDAT total exceeds %zu bytes", kMaxCompressedBytes);
                error = ImageError::kTooLarge;
                return nullptr;
            }
            compressed_size += chunk.length;
        }
        if (fseek(f, chunk.length + 4, SEEK_CUR) != 0) {  // Skip data + CRC
            break;
        }
    }

    if (compressed_size == 0) {
        ESP_LOGE(TAG, "PNG: no IDAT chunks");
        error = ImageError::kCorrupt;
        return nullptr;
    }

    uint8_t* compressed =
        static_cast<uint8_t*>(heap_caps_malloc(compressed_size, MALLOC_CAP_SPIRAM));
    if (compressed == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate %zu bytes for PNG IDAT", compressed_size);
        error = ImageError::kNoMemory;
        return nullptr;
    }

    if (fseek(f, idat_start, SEEK_SET) != 0) {
        heap_caps_free(compressed);
        error = ImageError::kCorrupt;
        return nullptr;
    }
    size_t filled = 0;
    while (filled < compressed_size && ReadPngChunk(f, chunk)) {
        if (chunk.type == 0x49454E44) {  // 'IEND'
            break;
        }
        // Re-apply the cap: the file could have changed between passes (SD card).
        if (chunk.length > kMaxCompressedBytes) {
            heap_caps_free(compressed);
            error = ImageError::kTooLarge;
            return nullptr;
        }
        if (chunk.type == 0x49444154) {  // 'IDAT'
            if (chunk.length > compressed_size - filled ||
                ReadChunk(f, compressed + filled, chunk.length) != chunk.length) {
                heap_caps_free(compressed);
                error = ImageError::kCorrupt;
                return nullptr;
            }
            filled += chunk.length;
            if (fseek(f, 4, SEEK_CUR) != 0) {  // Skip CRC
                break;
            }
        } else if (fseek(f, chunk.length + 4, SEEK_CUR) != 0) {  // Skip data + CRC
            break;
        }
    }
    if (filled != compressed_size) {
        heap_caps_free(compressed);
        error = ImageError::kCorrupt;
        return nullptr;
    }

    // Decompress with the ROM tinfl. Grayscale+alpha(4) is 2 bytes/pixel; getting
    // this wrong halves the stride and misaligns every scanline.
    size_t bytes_per_pixel = (color_type == 2)   ? 3
                             : (color_type == 6) ? 4
                             : (color_type == 4) ? 2
                                                 : 1;
    if (bit_depth == 16) {
        bytes_per_pixel *= 2;
    }
    size_t stride = width * bytes_per_pixel + 1;  // +1 for filter byte
    // 流式解码：不再分配整幅 raw 缓冲（stride*height 可能远超 8MB PSRAM）。
    // 改为 tinfl 环形缓冲 + 两个行缓冲，边 inflate 边 unfilter 边缩放。
    // compressed 交给 RAII 接管，之后任何返回路径都会自动释放它。
    PngScratch compressed_guard;
    compressed_guard.Adopt(compressed);

    // 缩放比例与输出尺寸（与 BMP/JPEG 一致，只缩不放）。
    int scale = 1;
    while (width / scale > (uint32_t)max_width || height / scale > (uint32_t)max_height) {
        scale++;
    }
    uint32_t out_width = width / scale;
    uint32_t out_height = height / scale;
    // 极端长宽比（如 10x8192）缩放后某维为 0，out_size=0 会让 malloc(0) 返回 NULL
    // 而误报"内存不足"。明确按过大/不支持处理。
    if (out_width == 0 || out_height == 0) {
        ESP_LOGW(TAG, "PNG: extreme aspect ratio, scaled dimension is zero");
        error = ImageError::kTooLarge;
        return nullptr;
    }
    size_t out_size = (size_t)out_width * out_height * 2;

    // tinfl 环形输出缓冲：必须恰为 TINFL_LZ_DICT_SIZE（32KB）。miniz 的 LZ 回溯
    // 按该常量取模（结构体里只有 m_dist_from_out_buf_start，没有缓冲尺寸字段），
    // 缓冲更大反而会破坏回溯引用。它与 stride 无关：行数据被增量 drain 到行缓冲。
    PngScratch ring_buf;
    uint8_t* ring = ring_buf.Alloc(TINFL_LZ_DICT_SIZE, MALLOC_CAP_SPIRAM);
    // 两个行缓冲：cur 累积当前行，prev 保存上一行已 unfilter 的像素。
    PngScratch row_a, row_b;
    uint8_t* buf_a = row_a.Alloc(stride, MALLOC_CAP_SPIRAM);
    uint8_t* buf_b = row_b.Alloc(stride, MALLOC_CAP_SPIRAM);
    // RGB565 输出位图（已缩放到屏幕大小，≤~150KB）。
    PngScratch out_buf;
    uint8_t* rgb565 = out_buf.Alloc(out_size, MALLOC_CAP_SPIRAM);
    if (ring == nullptr || buf_a == nullptr || buf_b == nullptr || rgb565 == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate PNG streaming buffers");
        error = ImageError::kNoMemory;
        return nullptr;
    }

    // 处理一整行：用 prev 行做 PNG unfilter（None/Sub/Up/Average/Paeth），再按
    // scale 采样转 RGB565 写入输出。row_y 为源行号；返回 false 表示 filter 非法。
    // cur[0] 是 filter 字节，cur[1..stride) 是像素；prev 同样布局。
    auto process_row = [&](uint8_t* cur, const uint8_t* prev, uint32_t row_y) -> bool {
        uint8_t filter = cur[0];
        if (filter > 4) {
            ESP_LOGE(TAG, "PNG: bad filter %u at row %lu", filter, row_y);
            return false;
        }
        bool has_up = (row_y > 0);
        if (filter != 0) {
            for (size_t x = 1; x < stride; x++) {
                uint8_t left = (x > bytes_per_pixel) ? cur[x - bytes_per_pixel] : 0;
                uint8_t up = has_up ? prev[x] : 0;
                uint8_t up_left = (has_up && x > bytes_per_pixel) ? prev[x - bytes_per_pixel] : 0;
                uint8_t v = cur[x];
                if (filter == 1) {  // Sub
                    v = v + left;
                } else if (filter == 2) {  // Up
                    v = v + up;
                } else if (filter == 3) {  // Average
                    v = v + ((left + up) / 2);
                } else {  // Paeth
                    int p = left + up - up_left;
                    int pa = abs(p - left), pb = abs(p - up), pc = abs(p - up_left);
                    uint8_t predictor = (pa <= pb && pa <= pc) ? left : (pb <= pc) ? up : up_left;
                    v = v + predictor;
                }
                cur[x] = v;
            }
        }
        // 仅在采样行写入输出位图。必须校验 out_y < out_height：out_height 是
        // floor(height/scale)，当 height 不能被 scale 整除时最后一个采样源行会映射到
        // out_y == out_height，越界写一行（堆溢出）。丢弃尾部不足一个采样块的源行。
        if (row_y % (uint32_t)scale == 0 && row_y / (uint32_t)scale < out_height) {
            uint32_t out_y = row_y / scale;
            uint8_t* dst = rgb565 + (size_t)out_y * out_width * 2;
            // 16-bit 时每通道 2 字节大端，取各通道高字节须按通道步长 cs 偏移，
            // 否则 RGB/RGBA 会读到 R_hi,R_lo,G_hi 而非 R_hi,G_hi,B_hi。
            const size_t cs = bit_depth / 8;
            for (uint32_t x = 0; x < out_width; x++) {
                uint32_t src_x = (x * scale) < width ? (x * scale) : (width - 1);
                const uint8_t* pixel = cur + 1 + (size_t)src_x * bytes_per_pixel;
                uint8_t r = 0, g = 0, b = 0;
                if (color_type == 0 || color_type == 4) {  // 灰度 / 灰度+alpha（忽略 alpha）
                    r = g = b = pixel[0];
                } else if (color_type == 2) {  // RGB
                    r = pixel[0];
                    g = pixel[cs];
                    b = pixel[2 * cs];
                } else {  // RGBA（忽略 alpha）
                    r = pixel[0];
                    g = pixel[cs];
                    b = pixel[2 * cs];
                }
                uint16_t rgb565_pixel = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
                dst[x * 2] = rgb565_pixel & 0xFF;
                dst[x * 2 + 1] = rgb565_pixel >> 8;
            }
        }
        return true;
    };

    // tinfl 流式 inflate（环形模式）。零初始化确保 m_dist_from_out_buf_start 从 0
    // 起，与 ring_ofs 同步推进，LZ 回溯取模才正确（ROM 测试也用 calloc 清零）。
    tinfl_decompressor inflator{};
    tinfl_init(&inflator);
    size_t in_pos = 0;
    size_t ring_ofs = 0;  // 环形缓冲写偏移 = 已产出字节数 % TINFL_LZ_DICT_SIZE
    size_t row_fill = 0;  // cur 行已累积字节数
    uint32_t row_y = 0;   // 已完成的源行数
    uint8_t* cur = buf_a;
    uint8_t* prev = buf_b;
    int status = TINFL_STATUS_NEEDS_MORE_INPUT;

    while (row_y < height &&
           (status == TINFL_STATUS_NEEDS_MORE_INPUT || status == TINFL_STATUS_HAS_MORE_OUTPUT)) {
        size_t in_avail = compressed_size - in_pos;
        // 只给出到环形缓冲末尾的连续空间，tinfl 单次产出不会跨越末尾。
        size_t out_avail = TINFL_LZ_DICT_SIZE - ring_ofs;
        size_t in_consumed = in_avail;
        size_t out_written = out_avail;

        // IDAT 是 zlib 流（RFC1950），必须 PARSE_ZLIB_HEADER；环形模式不设
        // USING_NON_WRAPPING_OUTPUT_BUF。仅在仍有输入时承诺 HAS_MORE_INPUT，
        // 否则截断流会让 tinfl 永远等待。
        uint32_t flags = TINFL_FLAG_PARSE_ZLIB_HEADER;
        if (in_pos < compressed_size) {
            flags |= TINFL_FLAG_HAS_MORE_INPUT;
        }

        status = tinfl_decompress(&inflator, compressed + in_pos, &in_consumed, ring,
                                  ring + ring_ofs, &out_written, flags);
        in_pos += in_consumed;

        if (status < 0) {
            ESP_LOGE(TAG, "PNG: inflate failed: %d", status);
            error = ImageError::kCorrupt;
            return nullptr;
        }
        // 零进展且非完成态：输入截断或流异常，退出而非死循环。
        if (in_consumed == 0 && out_written == 0 && status != TINFL_STATUS_DONE) {
            ESP_LOGE(TAG, "PNG: inflate stalled at row %lu", row_y);
            error = ImageError::kCorrupt;
            return nullptr;
        }

        // 把本次产出的连续字节 drain 进行缓冲；凑满 stride 字节就处理一行。
        // 一行可跨多次 inflate 调用（stride 可大于环形缓冲），row_fill 跨调用累积，
        // 因此行数据不会因环形回绕而丢失。
        const uint8_t* src = ring + ring_ofs;
        size_t remaining = out_written;
        while (remaining > 0 && row_y < height) {
            size_t need = stride - row_fill;
            size_t take = (remaining < need) ? remaining : need;
            memcpy(cur + row_fill, src, take);
            row_fill += take;
            src += take;
            remaining -= take;
            if (row_fill == stride) {
                if (!process_row(cur, prev, row_y)) {
                    error = ImageError::kCorrupt;
                    return nullptr;
                }
                uint8_t* tmp = cur;
                cur = prev;
                prev = tmp;
                row_y++;
                row_fill = 0;
            }
        }

        ring_ofs = (ring_ofs + out_written) & (TINFL_LZ_DICT_SIZE - 1);
    }

    if (row_y < height) {
        ESP_LOGE(TAG, "PNG: incomplete inflate: %lu/%lu rows", row_y, height);
        error = ImageError::kCorrupt;
        return nullptr;
    }

    // 所有行已在流式循环中 unfilter 并缩放完毕，这里只组装结果。
    // nothrow：异常被禁用，抛出的 new 会在 OOM 时 abort()。
    auto image = std::unique_ptr<DecodedImage>(new (std::nothrow) DecodedImage());
    if (image == nullptr) {
        error = ImageError::kNoMemory;
        return nullptr;
    }
    image->kind = DecodedImage::Kind::kBitmap;
    image->width = out_width;
    image->height = out_height;
    image->data = out_buf.Release();  // 输出位图所有权转交给 DecodedImage
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
    // Check available PSRAM before attempting decode
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t min_required = 2 * 1024 * 1024;  // Require at least 2 MB free
    if (free_psram < min_required) {
        ESP_LOGW(TAG, "Insufficient PSRAM: %zu bytes free (need %zu)", free_psram, min_required);
        error = ImageError::kNoMemory;
        return nullptr;
    }

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
    ESP_LOGI(TAG, "Decoding %s: format=%d, size=%zu, free_psram=%zu", path.c_str(), (int)format,
             file_size, heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

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
