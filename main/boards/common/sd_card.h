#ifndef SD_CARD_H
#define SD_CARD_H

#include <driver/gpio.h>
#include <esp_err.h>
#include <sdmmc_cmd.h>

#include <atomic>
#include <cstdint>
#include <string>

// microSD card on the SDMMC host, mounted as a FAT filesystem.
//
// ESP-IDF FATFS is built without exFAT, so cards must be formatted as FAT32 (or FAT16).
// Cards larger than 32 GB ship as exFAT and need reformatting first.
//
// Mount() and the destructor must run on one task (normally the main task).
// IsMounted(), last_error() and mount_point() are safe from any task.
class SdmmcCard {
public:
    struct Config {
        const char* mount_point;
        gpio_num_t clk;
        gpio_num_t cmd;
        gpio_num_t d0;
        // Set d1..d3 for 4-bit mode; leave them unconnected for 1-bit mode.
        gpio_num_t d1 = GPIO_NUM_NC;
        gpio_num_t d2 = GPIO_NUM_NC;
        gpio_num_t d3 = GPIO_NUM_NC;
        int max_files = 5;
        // When true, FAT is managed by an external owner (TinyUSB MSC) instead of this
        // class: Mount() only initializes the bare card, and IsMounted() tracks the owner's
        // APP/USB mount point via SetAppMounted(). Used by the MetaOS USB U-disk feature.
        bool usb_msc = false;
    };

    explicit SdmmcCard(const Config& config);
    ~SdmmcCard();

    SdmmcCard(const SdmmcCard&) = delete;
    SdmmcCard& operator=(const SdmmcCard&) = delete;

    // Mounts the card if it is not mounted yet. Failed attempts are rate limited unless
    // `force` is set, so callers can retry on demand without stalling each time.
    bool Mount(bool force = false);
    bool IsMounted() const { return mounted_.load(); }
    // Result of the last mount attempt. ESP_FAIL means the card answered but carries no FAT
    // filesystem (for example exFAT); ESP_ERR_INVALID_STATE means no attempt was made yet.
    esp_err_t last_error() const { return last_error_.load(); }
    const std::string& mount_point() const { return mount_point_; }
    bool GetUsage(uint64_t& total_bytes, uint64_t& free_bytes) const;

    // Raw card descriptor for the TinyUSB MSC storage. Valid after a successful Mount() in
    // usb_msc mode; the card stays initialized until destruction.
    sdmmc_card_t* card() const { return card_; }
    bool usb_msc() const { return config_.usb_msc; }
    // External FAT owner (TinyUSB MSC) reports whether the card is mounted for the app.
    // Only meaningful in usb_msc mode. Safe from any task (updates an atomic).
    void SetAppMounted(bool mounted);
    // External FAT owner reports a filesystem error (e.g. ESP_FAIL when the card has no FAT),
    // so CardState can report kNoFilesystem. Only meaningful in usb_msc mode.
    void SetLastError(esp_err_t err);

private:
    bool MountBare(bool force);
    Config config_;
    std::string mount_point_;
    sdmmc_card_t* card_ = nullptr;
    bool card_inited_ = false;  // usb_msc mode: bare card initialized, FAT owned externally
    std::atomic<bool> mounted_{false};
    std::atomic<esp_err_t> last_error_{ESP_ERR_INVALID_STATE};
    int64_t last_attempt_us_ = 0;
};

#endif  // SD_CARD_H
