#include "metaos_speech.h"

#if CONFIG_METAOS_ENABLE

#include <esp_log.h>
#include <sys/stat.h>

#include "application.h"
#include "metaos_common.h"
#include "metaos_worker.h"

#define TAG "MetaSpeech"

// ESP-SR TTS would be included here when available:
// #include <esp_tts.h>
// #include <esp_tts_voice_xiaole.h>

namespace metaos {

namespace {
constexpr const char* kVoiceDataPath = "/sdcard/esp_tts_voice_data_xiaole.dat";
constexpr const char* kOutputPath = "/sdcard/metaos/speech.ogg";
constexpr size_t kMaxTextBytes = 200;
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
        // TODO: Actual TTS synthesis would go here when ESP-SR component is available
        //
        // Pseudocode for integration:
        // 1. Load voice data: esp_tts_handle_t tts = esp_tts_create(voice_data)
        // 2. Synthesize PCM: esp_tts_parse_chinese(tts, text_copy, &pcm_data, &pcm_len)
        // 3. Encode to Opus with 60ms frames
        // 4. Write Ogg container with proper headers and pages
        // 5. Schedule playback via Application::StartLocalPlayback()

        ESP_LOGW(TAG, "TTS synthesis not implemented - ESP-SR component required");
        ESP_LOGW(TAG, "Would synthesize: %s", text_copy.c_str());
        ESP_LOGW(TAG, "Would write to: %s", output_copy.c_str());

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
