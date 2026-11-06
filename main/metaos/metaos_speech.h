#ifndef METAOS_SPEECH_H
#define METAOS_SPEECH_H

#include <sdkconfig.h>

#if CONFIG_METAOS_ENABLE

#include <functional>
#include <string>
#include <vector>

namespace metaos {

class Storage;

// Offline TTS for reminders using ESP-SR's esp_tts_chinese.
// Synthesizes UTF-8 text to 16kHz mono PCM, encodes to Opus, writes Ogg container,
// and plays via Application::StartLocalPlayback().
class SpeechSynthesizer {
public:
    explicit SpeechSynthesizer(Storage& storage);
    ~SpeechSynthesizer();

    SpeechSynthesizer(const SpeechSynthesizer&) = delete;
    SpeechSynthesizer& operator=(const SpeechSynthesizer&) = delete;

    // Check if the voice data file exists at /sdcard/esp_tts_voice_data_xiaole.dat
    bool IsVoiceDataAvailable();

    // Synthesize text to /sdcard/metaos/speech.ogg, then schedule playback on the main task.
    // Returns false if voice data is missing, worker queue is full, or text is too long.
    // Text is prefixed with "提醒：" and capped at ~200 bytes.
    bool SynthesizeAndPlay(const std::vector<std::string>& messages);

private:
    Storage& storage_;
};

}  // namespace metaos

#endif  // CONFIG_METAOS_ENABLE
#endif  // METAOS_SPEECH_H
