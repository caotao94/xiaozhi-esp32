#include "firmware_repo.h"

#include <dirent.h>
#include <esp_log.h>
#include <psa/crypto.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cJSON.h"
#include "firmware_pubkey.h"

#define TAG "FwRepo"

namespace firmware {

namespace {

constexpr size_t kMaxManifestBytes = 256 * 1024;
constexpr size_t kIoBufBytes = 4096;

std::string DirName(const std::string& path) {
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string BaseName(const std::string& path) {
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool IsDirectory(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool FileExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string JsonStr(const cJSON* obj, const char* key, const std::string& def = "") {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring != nullptr) {
        return item->valuestring;
    }
    return def;
}

uint64_t JsonU64(const cJSON* obj, const char* key, uint64_t def = 0) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item)) {
        double v = item->valuedouble;
        // Avoid UB from negative / out-of-range / non-finite double->uint64 casts.
        // Use strict < 2^64: the double form of UINT64_MAX rounds up to 2^64.
        if (std::isfinite(v) && v >= 0.0 && v < 0x1p64) {
            return static_cast<uint64_t>(v);
        }
    }
    return def;
}

// Reject pathologically nested JSON before parsing (cJSON recurses; a malformed
// manifest from an untrusted card must not overflow the worker stack).
bool JsonNestingOk(const std::string& text, int max_depth) {
    int depth = 0;
    bool in_str = false;
    bool esc = false;
    for (char c : text) {
        if (in_str) {
            if (esc) {
                esc = false;
            } else if (c == '\\') {
                esc = true;
            } else if (c == '"') {
                in_str = false;
            }
            continue;
        }
        if (c == '"') {
            in_str = true;
        } else if (c == '{' || c == '[') {
            if (++depth > max_depth) {
                return false;
            }
        } else if (c == '}' || c == ']') {
            --depth;
        }
    }
    return true;
}

bool JsonBool(const cJSON* obj, const char* key, bool def = false) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item);
    }
    return def;
}

std::string ToHex(const uint8_t* data, size_t len) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; i++) {
        out.push_back(kHex[data[i] >> 4]);
        out.push_back(kHex[data[i] & 0x0f]);
    }
    return out;
}

bool EqualsIgnoreCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); i++) {
        if (tolower(static_cast<unsigned char>(a[i])) !=
            tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

}  // namespace

bool FirmwareRepo::ParseManifest(const std::string& manifest_path, FirmwareEntry& entry,
                                 std::string& err) {
    FILE* f = fopen(manifest_path.c_str(), "rb");
    if (f == nullptr) {
        err = "cannot open manifest";
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || static_cast<size_t>(size) > kMaxManifestBytes) {
        fclose(f);
        err = "manifest size invalid";
        return false;
    }
    std::string text;
    text.resize(static_cast<size_t>(size));
    size_t got = fread(&text[0], 1, text.size(), f);
    fclose(f);
    if (got != text.size()) {
        err = "manifest read failed";
        return false;
    }

    if (!JsonNestingOk(text, 64)) {
        err = "manifest nesting too deep";
        return false;
    }
    cJSON* root = cJSON_ParseWithLength(text.c_str(), text.size());
    if (root == nullptr) {
        err = "manifest is not valid JSON";
        return false;
    }

    entry.manifest_path = manifest_path;
    entry.firmware_id = JsonStr(root, "firmware_id");
    entry.name = JsonStr(root, "firmware_name", entry.firmware_id);
    entry.version = JsonStr(root, "version");
    entry.build_number = static_cast<uint32_t>(JsonU64(root, "build_number"));
    entry.target_chip = JsonStr(root, "target_chip");
    entry.target_board = JsonStr(root, "target_board");
    entry.min_bootloader_version = JsonStr(root, "min_bootloader_version");
    entry.channel = JsonStr(root, "release_channel");
    entry.description = JsonStr(root, "description");
    entry.release_time = JsonStr(root, "release_time");
    entry.size = JsonU64(root, "firmware_size");
    entry.sha256 = JsonStr(root, "firmware_sha256");
    entry.signature = JsonStr(root, "signature");
    entry.key_id = static_cast<int>(JsonU64(root, "key_id"));
    entry.rollback_supported = JsonBool(root, "rollback_supported");
    entry.has_signature = !entry.signature.empty();
    entry.signature_verified = false;  // Phase 4

    std::string bin_file = JsonStr(root, "file");
    if (bin_file.empty()) {
        bin_file = entry.firmware_id.empty() ? "firmware.bin" : entry.firmware_id + ".bin";
    }
    // Prevent escaping the repository directory via ".." components.
    if (bin_file.find("..") != std::string::npos || bin_file.front() == '/') {
        cJSON_Delete(root);
        err = "unsafe firmware file path";
        return false;
    }
    std::string dir = DirName(manifest_path);
    entry.bin_path = dir + "/" + bin_file;

    cJSON_Delete(root);

    if (entry.firmware_id.empty() && entry.name.empty()) {
        err = "manifest missing firmware_id/name";
        return false;
    }
    if (entry.version.empty()) {
        err = "manifest missing version";
        return false;
    }
    if (!FileExists(entry.bin_path)) {
        err = "firmware bin not found: " + BaseName(entry.bin_path);
        return false;
    }
    if (entry.size == 0) {
        struct stat st;
        if (stat(entry.bin_path.c_str(), &st) == 0) {
            entry.size = static_cast<uint64_t>(st.st_size);
        }
    }
    return true;
}

std::vector<FirmwareEntry> FirmwareRepo::Scan(const std::string& root) {
    std::vector<FirmwareEntry> entries;
    DIR* dir = opendir(root.c_str());
    if (dir == nullptr) {
        ESP_LOGW(TAG, "repository root not found: %s", root.c_str());
        return entries;
    }
    struct dirent* de;
    while ((de = readdir(dir)) != nullptr) {
        std::string name = de->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        std::string sub = root + "/" + name;
        bool is_dir = false;
#ifdef DT_DIR
        is_dir = (de->d_type == DT_DIR);
        if (de->d_type == DT_UNKNOWN) {
            is_dir = IsDirectory(sub);
        }
#else
        is_dir = IsDirectory(sub);
#endif
        if (!is_dir) {
            continue;
        }
        std::string manifest = sub + "/manifest.json";
        if (!FileExists(manifest)) {
            continue;
        }
        FirmwareEntry entry;
        std::string err;
        if (ParseManifest(manifest, entry, err)) {
            if (entry.channel.empty()) {
                entry.channel = name;
            }
            entries.push_back(std::move(entry));
        } else {
            ESP_LOGW(TAG, "skip %s: %s", manifest.c_str(), err.c_str());
            entry.error = err;
            entry.manifest_path = manifest;
            entry.channel = name;
            entries.push_back(std::move(entry));
        }
    }
    closedir(dir);
    std::sort(entries.begin(), entries.end(),
              [](const FirmwareEntry& a, const FirmwareEntry& b) { return a.name < b.name; });
    ESP_LOGI(TAG, "scanned %s: %zu entries", root.c_str(), entries.size());
    return entries;
}

bool FirmwareRepo::Sha256File(const std::string& path, std::string& hex,
                              const ProgressFn& progress, std::string* err) {
    auto fail = [&](const char* msg) {
        if (err != nullptr) {
            *err = msg;
        }
        return false;
    };
    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return fail("cannot open file");
    }
    fseek(f, 0, SEEK_END);
    long total = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (total < 0) {
        fclose(f);
        return fail("cannot size file");
    }

    // PSA Crypto multi-part SHA-256 (mbedTLS 4.x; legacy mbedtls/sha256.h is private).
    if (psa_crypto_init() != PSA_SUCCESS) {
        fclose(f);
        return fail("psa crypto init failed");
    }
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&op, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        fclose(f);
        return fail("sha256 setup failed");
    }

    uint8_t* buf = static_cast<uint8_t*>(malloc(kIoBufBytes));
    if (buf == nullptr) {
        psa_hash_abort(&op);
        fclose(f);
        return fail("out of memory");
    }
    uint64_t done = 0;
    bool ok = true;
    bool cancelled = false;
    while (done < static_cast<uint64_t>(total)) {
        size_t want = static_cast<size_t>(
            std::min<uint64_t>(kIoBufBytes, static_cast<uint64_t>(total) - done));
        size_t got = fread(buf, 1, want, f);
        if (got == 0) {
            ok = false;
            break;
        }
        if (psa_hash_update(&op, buf, got) != PSA_SUCCESS) {
            ok = false;
            break;
        }
        done += got;
        if (progress && !progress(done, static_cast<uint64_t>(total))) {
            ok = false;
            cancelled = true;
            break;
        }
    }
    free(buf);
    fclose(f);

    uint8_t digest[PSA_HASH_LENGTH(PSA_ALG_SHA_256)];
    size_t digest_len = 0;
    if (ok && psa_hash_finish(&op, digest, sizeof(digest), &digest_len) != PSA_SUCCESS) {
        ok = false;
    }
    if (!ok || digest_len != PSA_HASH_LENGTH(PSA_ALG_SHA_256)) {
        psa_hash_abort(&op);
        return fail(cancelled ? "cancelled" : "sha256 read failed");
    }
    hex = ToHex(digest, digest_len);
    return true;
}

bool FirmwareRepo::Verify(FirmwareEntry& entry, const ProgressFn& progress) {
    entry.sha256_checked = true;
    entry.sha256_ok = false;
    if (entry.sha256.empty()) {
        entry.error = "manifest has no firmware_sha256";
        return false;
    }
    std::string hex;
    std::string err;
    if (!Sha256File(entry.bin_path, hex, progress, &err)) {
        entry.error = err;
        return false;
    }
    entry.actual_sha256 = hex;
    entry.sha256_ok = EqualsIgnoreCase(hex, entry.sha256);
    if (!entry.sha256_ok) {
        entry.error = "SHA-256 mismatch";
    }
    return entry.sha256_ok;
}

bool FirmwareRepo::VerifySignature(FirmwareEntry& entry) {
    entry.signature_verified = false;
    if (!entry.has_signature) {
        return true;  // nothing to verify; caller decides if unsigned is allowed
    }
#if METAOS_FW_PUBKEY_PROVISIONED
    // Single embedded key: reject manifests that name a different key id.
    if (entry.key_id != METAOS_FW_KEY_ID) {
        ESP_LOGW(TAG, "manifest key_id %d != embedded %d", entry.key_id, METAOS_FW_KEY_ID);
        return false;
    }
    // PSA must be initialized before any hash/verify call (independent of whether
    // Sha256File happened to run earlier this boot).
    if (psa_crypto_init() != PSA_SUCCESS) {
        return false;
    }
    // Resolve the detached signature relative to the manifest directory, with the
    // same path-safety policy as the firmware bin (no "..", no absolute path).
    const std::string& sig_rel = entry.signature;
    if (sig_rel.find("..") != std::string::npos || (!sig_rel.empty() && sig_rel.front() == '/')) {
        ESP_LOGW(TAG, "unsafe signature path");
        return false;
    }
    std::string sig_path = DirName(entry.manifest_path) + "/" + sig_rel;
    FILE* sf = fopen(sig_path.c_str(), "rb");
    if (sf == nullptr) {
        ESP_LOGW(TAG, "signature file missing: %s", sig_path.c_str());
        return false;
    }
    fseek(sf, 0, SEEK_END);
    long sig_size = ftell(sf);
    fseek(sf, 0, SEEK_SET);
    uint8_t sig[64];
    size_t sig_len = fread(sig, 1, sizeof(sig), sf);
    fclose(sf);
    if (sig_size != 64 || sig_len != sizeof(sig)) {
        ESP_LOGW(TAG, "signature must be exactly 64 bytes (r||s)");
        return false;
    }

    // Hash the exact manifest bytes.
    FILE* mf = fopen(entry.manifest_path.c_str(), "rb");
    if (mf == nullptr) {
        return false;
    }
    fseek(mf, 0, SEEK_END);
    long msize = ftell(mf);
    fseek(mf, 0, SEEK_SET);
    if (msize <= 0 || msize > static_cast<long>(kMaxManifestBytes)) {
        fclose(mf);
        return false;
    }
    std::string manifest;
    manifest.resize(static_cast<size_t>(msize));
    size_t got = fread(&manifest[0], 1, manifest.size(), mf);
    fclose(mf);
    if (got != manifest.size()) {
        return false;
    }
    uint8_t hash[32];
    size_t hash_len = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, reinterpret_cast<const uint8_t*>(manifest.data()),
                         manifest.size(), hash, sizeof(hash), &hash_len) != PSA_SUCCESS ||
        hash_len != 32) {
        return false;
    }

    // Import the embedded public key (uncompressed point 0x04 || X || Y).
    uint8_t pub[65];
    pub[0] = 0x04;
    memcpy(pub + 1, kMetaosFwPublicKey, 64);
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_VERIFY_HASH);
    psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr, 256);
    psa_key_id_t key = 0;
    if (psa_import_key(&attr, pub, sizeof(pub), &key) != PSA_SUCCESS) {
        return false;
    }
    psa_status_t st = psa_verify_hash(key, PSA_ALG_ECDSA(PSA_ALG_SHA_256), hash, hash_len,
                                      sig, sizeof(sig));
    psa_destroy_key(key);
    entry.signature_verified = (st == PSA_SUCCESS);
    if (!entry.signature_verified) {
        ESP_LOGW(TAG, "manifest signature verification failed: %d", static_cast<int>(st));
    }
    return entry.signature_verified;
#else
    ESP_LOGW(TAG, "signature present but no public key provisioned");
    return false;
#endif
}

}  // namespace firmware
