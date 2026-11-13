#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace firmware {

// One firmware image described by a manifest.json in the TF-card repository.
struct FirmwareEntry {
    std::string firmware_id;
    std::string name;
    std::string version;
    uint32_t build_number = 0;
    std::string target_chip;
    std::string target_board;
    std::string min_bootloader_version;
    std::string channel;
    std::string description;
    std::string release_time;
    uint64_t size = 0;           // firmware_size from manifest
    std::string sha256;          // firmware_sha256 from manifest (expected)
    std::string bin_path;        // absolute path to the .bin
    std::string manifest_path;   // absolute path to manifest.json
    std::string signature;       // detached signature reference (Phase 4)
    int key_id = 0;
    bool rollback_supported = false;

    // Verification state (filled by Verify(), not by the manifest).
    bool has_signature = false;
    bool signature_verified = false;  // always false until Phase 4 implements it
    bool sha256_checked = false;
    bool sha256_ok = false;
    std::string actual_sha256;
    std::string error;           // scan/parse error, empty when ok
};

// Progress callback: (bytes_done, bytes_total). Return false to abort.
using ProgressFn = std::function<bool(uint64_t, uint64_t)>;

// Read-only access to the TF-card firmware repository. Never formats or writes
// the card. Layout: <root>/<channel>/manifest.json + <bin>, e.g.
// /sdcard/metaos/firmware/stable/manifest.json.
class FirmwareRepo {
public:
    // Scans every immediate subdirectory of root for a manifest.json.
    std::vector<FirmwareEntry> Scan(const std::string& root);

    // Parses one manifest file; bin_path is resolved relative to its directory.
    static bool ParseManifest(const std::string& manifest_path, FirmwareEntry& entry,
                              std::string& err);

    // Streaming SHA-256 of a file (4KB buffer, no whole-file load). Lowercase hex.
    static bool Sha256File(const std::string& path, std::string& hex,
                           const ProgressFn& progress = nullptr, std::string* err = nullptr);

    // Computes SHA-256 and compares with the manifest expectation.
    static bool Verify(FirmwareEntry& entry, const ProgressFn& progress = nullptr);

    // Verifies the detached manifest signature (ECDSA P-256 / SHA-256) with the
    // embedded public key. Sets entry.signature_verified. Returns true when the
    // signature is valid OR absent; false when a signature is present but invalid
    // or cannot be verified (no provisioned key). Absent signature is not an error
    // here; the caller decides whether unsigned images are allowed.
    static bool VerifySignature(FirmwareEntry& entry);
};

}  // namespace firmware
