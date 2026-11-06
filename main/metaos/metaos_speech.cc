#include "metaos_speech.h"

#if CONFIG_METAOS_ENABLE

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_rom_crc.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstring>

#include "application.h"
#include "encoder/impl/esp_opus_enc.h"
#include "esp_tts.h"
#include "esp_tts_voice_xiaole.h"
#include "metaos_common.h"
#include "metaos_worker.h"

#define TAG "MetaSpeech"

namespace metaos {

namespace {
constexpr const char* kVoiceDataPath = "/sdcard/esp_tts_voice_data_xiaole.dat";
constexpr const char* kOutputPath = "/sdcard/metaos/speech.ogg";
constexpr size_t kMaxTextBytes = 200;
constexpr int kTtsSampleRate = 16000;
constexpr int kOpusSampleRate = 16000;
constexpr int kTtsSpeed = 0;  // 0 = slowest, 5 = fastest

// Simple Ogg page writer for Opus streams
class OggWriter {
   public:
    explicit OggWriter(FILE* f) : file_(f) {}

    bool WriteOpusHeaders(int sample_rate, int channels) {
        channels_ = channels;
        // OpusHead packet
        uint8_t head[19] = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 0, 0, 0, 0x80, 0x3e,
                            0,   0,   0,   0};
        head[9] = static_cast<uint8_t>(channels);
        head[12] = sample_rate & 0xff;
        head[13] = (sample_rate >> 8) & 0xff;
        head[14] = (sample_rate >> 16) & 0xff;
        head[15] = (sample_rate >> 24) & 0xff;
        if (!WritePage(head, sizeof(head), 0, 2)) {  // BOS flag
            return false;
        }

        // OpusTags packet (minimal)
        uint8_t tags[20] = {'O', 'p', 'u', 's', 'T', 'a', 'g', 's', 0, 0, 0, 0, 0, 0, 0, 0};
        return WritePage(tags, sizeof(tags), 0, 0);
    }

    bool WriteOpusPacket(const uint8_t* data, size_t size, uint64_t granule) {
        return WritePage(data, size, granule, granule == last_granule_ ? 4 : 0);  // EOS on last
    }

    void SetLastGranule(uint64_t g) { last_granule_ = g; }

   private:
    bool WritePage(const uint8_t* packet, size_t packet_size, uint64_t granule, uint8_t flags) {
        uint8_t page[282];  // Max page header is 27 + 255
        page[0] = 'O';
        page[1] = 'g';
        page[2] = 'g';
        page[3] = 'S';
        page[4] = 0;                       // version
        page[5] = flags;                   // header type
        memcpy(page + 6, &granule, 8);     // granule position
        memset(page + 14, 0, 4);           // serial
        memcpy(page + 18, &seq_, 4);       // page sequence
        memset(page + 22, 0, 4);           // checksum (fill later)
        page[26] = 1;                      // 1 segment
        page[27] = packet_size & 0xff;     // segment size
        uint32_t crc = 0;
        crc = esp_rom_crc32_le(crc, page, 28);
        crc = esp_rom_crc32_le(crc, packet, packet_size);
        memcpy(page + 22, &crc, 4);
        seq_++;
        return fwrite(page, 28, 1, file_) == 1 && fwrite(packet, packet_size, 1, file_) == 1;
    }

    FILE* file_;
    int channels_ = 1;
    uint32_t seq_ = 0;
    uint64_t last_granule_ = 0;
};

}  // namespace

SpeechSynthesizer::SpeechSynthesizer(Storage& storage) : storage_(storage) {}

SpeechSynthesizer::~SpeechSynthesizer() = default;

bool SpeechSynthesizer::IsVoiceDataAvailable() {
    if (!storage_.IsMounted()) {
        return false;
    }
    struct stat st;
    return stat(kVoiceDataPath, &st) == 0 && S_ISREG(st.st_mode);
}

bool SpeechSynthesizer::SynthesizeAndPlay(const std::vector<std::string>& messages) {
    if (!IsVoiceDataAvailable()) {
        ESP_LOGW(TAG, "Voice data file not found: %s", kVoiceDataPath);
        return false;
    }

    if (messages.empty()) {
        return false;
    }

    // Combine messages with prefix
    std::string text = "提醒：";
    for (size_t i = 0; i < messages.size(); i++) {
        if (i > 0) {
            text += "；";
        }
        text += messages[i];
        if (text.size() > kMaxTextBytes) {
            text.resize(kMaxTextBytes);
            break;
        }
    }

    ESP_LOGI(TAG, "Synthesizing: %s", text.c_str());

    // Copy text for worker
    std::string text_copy = text;
    std::string output_copy = kOutputPath;

    // Synthesize on worker thread
    bool posted = PostWork([text_copy, output_copy]() {
        // Load voice data from SD card
        FILE* voice_file = fopen(kVoiceDataPath, "rb");
        if (!voice_file) {
            ESP_LOGE(TAG, "Failed to open voice data: %s", kVoiceDataPath);
            return;
        }

        fseek(voice_file, 0, SEEK_END);
        size_t voice_size = ftell(voice_file);
        fseek(voice_file, 0, SEEK_SET);

        void* voice_data = heap_caps_malloc(voice_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!voice_data) {
            ESP_LOGE(TAG, "Failed to allocate %zu bytes for voice data", voice_size);
            fclose(voice_file);
            return;
        }

        if (fread(voice_data, 1, voice_size, voice_file) != voice_size) {
            ESP_LOGE(TAG, "Failed to read voice data");
            heap_caps_free(voice_data);
            fclose(voice_file);
            return;
        }
        fclose(voice_file);

        // Initialize TTS voice set
        esp_tts_voice_t* voice = esp_tts_voice_set_init(&esp_tts_voice_xiaole, voice_data);
        if (!voice) {
            ESP_LOGE(TAG, "Failed to init voice set");
            heap_caps_free(voice_data);
            return;
        }

        // Create TTS handle
        esp_tts_handle_t tts = esp_tts_create(voice);
        if (!tts) {
            ESP_LOGE(TAG, "Failed to create TTS handle");
            esp_tts_voice_set_free(voice);
            heap_caps_free(voice_data);
            return;
        }

        // Parse Chinese text
        if (esp_tts_parse_chinese(tts, text_copy.c_str()) == 0) {
            ESP_LOGE(TAG, "Failed to parse Chinese text");
            esp_tts_destroy(tts);
            esp_tts_voice_set_free(voice);
            heap_caps_free(voice_data);
            return;
        }

        // Create Opus encoder (16kHz mono, 20ms frames for speech)
        esp_opus_enc_config_t opus_cfg = {
            .sample_rate = kOpusSampleRate,
            .channel = 1,
            .bits_per_sample = 16,
            .bitrate = 24000,  // 24kbps for speech
            .frame_duration = ESP_OPUS_ENC_FRAME_DURATION_20_MS,
            .application_mode = ESP_OPUS_ENC_APPLICATION_VOIP,
            .complexity = 0,
            .enable_fec = false,
            .enable_dtx = false,
            .enable_vbr = false,
        };

        void* opus_enc = nullptr;
        if (esp_opus_enc_open(&opus_cfg, sizeof(opus_cfg), &opus_enc) != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "Failed to create Opus encoder");
            esp_tts_destroy(tts);
            esp_tts_voice_set_free(voice);
            heap_caps_free(voice_data);
            return;
        }

        int in_size = 0, out_size = 0;
        esp_opus_enc_get_frame_size(opus_enc, &in_size, &out_size);
        ESP_LOGI(TAG, "Opus frame: in=%d out=%d", in_size, out_size);

        // Open output file
        FILE* out_file = fopen(output_copy.c_str(), "wb");
        if (!out_file) {
            ESP_LOGE(TAG, "Failed to create output file: %s", output_copy.c_str());
            esp_opus_enc_close(opus_enc);
            esp_tts_destroy(tts);
            esp_tts_voice_set_free(voice);
            heap_caps_free(voice_data);
            return;
        }

        OggWriter ogg(out_file);
        if (!ogg.WriteOpusHeaders(kOpusSampleRate, 1)) {
            ESP_LOGE(TAG, "Failed to write Ogg headers");
            fclose(out_file);
            esp_opus_enc_close(opus_enc);
            esp_tts_destroy(tts);
            esp_tts_voice_set_free(voice);
            heap_caps_free(voice_data);
            return;
        }

        // Stream TTS output and encode to Opus
        uint8_t* opus_out = static_cast<uint8_t*>(malloc(out_size));
        if (!opus_out) {
            ESP_LOGE(TAG, "Failed to allocate Opus output buffer");
            fclose(out_file);
            esp_opus_enc_close(opus_enc);
            esp_tts_destroy(tts);
            esp_tts_voice_set_free(voice);
            heap_caps_free(voice_data);
            return;
        }

        uint64_t granule_pos = 0;
        int total_samples = 0;
        bool success = true;

        while (success) {
            int len = 0;
            short* pcm = esp_tts_stream_play(tts, &len, kTtsSpeed);
            if (len == 0) {
                break;  // End of synthesis
            }

            total_samples += len;

            // Encode PCM to Opus
            esp_audio_enc_in_frame_t in_frame = {
                .buffer = reinterpret_cast<uint8_t*>(pcm),
                .len = static_cast<uint32_t>(len * 2),  // 16-bit samples
            };

            esp_audio_enc_out_frame_t out_frame = {
                .buffer = opus_out,
                .len = static_cast<uint32_t>(out_size),
            };

            esp_audio_err_t ret = esp_opus_enc_process(opus_enc, &in_frame, &out_frame);
            if (ret == ESP_AUDIO_ERR_OK && out_frame.encoded_bytes > 0) {
                granule_pos += (len * 48000) / kTtsSampleRate;  // Opus always uses 48kHz granule
                if (!ogg.WriteOpusPacket(opus_out, out_frame.encoded_bytes, granule_pos)) {
                    ESP_LOGE(TAG, "Failed to write Opus packet");
                    success = false;
                    break;
                }
            } else if (ret != ESP_AUDIO_ERR_DATA_LACK) {
                ESP_LOGE(TAG, "Opus encode failed: %d", ret);
                success = false;
                break;
            }
        }

        // Mark last page with EOS
        ogg.SetLastGranule(granule_pos);

        free(opus_out);
        fclose(out_file);
        esp_opus_enc_close(opus_enc);
        esp_tts_destroy(tts);
        esp_tts_voice_set_free(voice);
        heap_caps_free(voice_data);

        if (success) {
            ESP_LOGI(TAG, "Synthesis complete: %d samples, %llu granules", total_samples,
                     granule_pos);

            // Schedule playback on main task
            Application::GetInstance().Schedule([output_copy, text_copy]() {
                Application::GetInstance().StartLocalPlayback(output_copy, text_copy, nullptr);
            });
        } else {
            remove(output_copy.c_str());
        }

        // For now, just log and don't attempt playback
        // When implemented:
        // Application::GetInstance().Schedule([]() {
        //     Application::GetInstance().StartLocalPlayback(output_copy);
        // });
    });

    if (!posted) {
        ESP_LOGE(TAG, "Failed to post synthesis work");
        return false;
    }

    return true;
}

}  // namespace metaos

#endif  // CONFIG_METAOS_ENABLE
