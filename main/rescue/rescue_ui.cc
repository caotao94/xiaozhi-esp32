#include "sdkconfig.h"

#if CONFIG_METAOS_RESCUE_MODE

#include "rescue_ui.h"

#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <lvgl.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "firmware_install.h"
#include "firmware_repo.h"
#include "lichuang_rescue_board.h"

#define TAG "RescueUI"

LV_FONT_DECLARE(font_rescue_20_4);

namespace {

constexpr const char* kRepoRoot = "/sdcard/metaos/firmware";

RescueBoard* g_board = nullptr;
const lv_font_t* g_font = &font_rescue_20_4;

// g_entries is only touched from the LVGL task.
std::vector<firmware::FirmwareEntry> g_entries;

lv_obj_t* g_status_box = nullptr;
lv_obj_t* g_repo_list = nullptr;
lv_obj_t* g_scan_btn = nullptr;
lv_obj_t* g_progress_panel = nullptr;
lv_obj_t* g_progress_bar = nullptr;
lv_obj_t* g_progress_label = nullptr;

// ---- Background worker (single task: SdmmcCard::Mount must stay on one task) ----
enum JobType { JOB_SCAN, JOB_INSTALL };
struct Job {
    JobType type;
    firmware::FirmwareEntry entry;
    const esp_partition_t* target;
};
QueueHandle_t g_jobs = nullptr;

std::mutex g_scan_mutex;
std::vector<firmware::FirmwareEntry> g_scan_result;
std::atomic<int> g_scan_state{0};  // 0 idle, 1 running, 2 done

std::atomic<uint64_t> g_done{0};
std::atomic<uint64_t> g_total{0};
std::atomic<int> g_phase{0};  // 0 idle, 1 verify, 2 write, 3 ok, 4 fail
std::atomic<bool> g_cancel{false};
char g_err[160] = {0};
firmware::FirmwareEntry g_inst_entry;
const esp_partition_t* g_inst_target = nullptr;
bool g_installing = false;

lv_obj_t* AddLabel(lv_obj_t* parent, const char* text) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(label, LV_PCT(100));
    return label;
}

lv_obj_t* AddButton(lv_obj_t* parent, const char* text, lv_event_cb_t cb, void* user_data) {
    lv_obj_t* btn = lv_button_create(parent);
    lv_obj_t* label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    if (cb != nullptr) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    }
    return btn;
}

const esp_partition_t* OtaPartition(esp_partition_subtype_t subtype) {
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, subtype, nullptr);
}

void FormatSize(uint64_t bytes, char* out, size_t n) {
    if (bytes >= 1024 * 1024) {
        snprintf(out, n, "%.1f MB", bytes / 1048576.0);
    } else {
        snprintf(out, n, "%.0f KB", bytes / 1024.0);
    }
}

void SetFont(lv_obj_t* obj) { lv_obj_set_style_text_font(obj, g_font, 0); }

void OnEntryClicked(lv_event_t* e);

// ---------------- Status ----------------

void RefreshStatus() {
    if (g_status_box == nullptr) {
        return;
    }
    lv_obj_clean(g_status_box);
    firmware::SlotInfo running = firmware::FirmwareInstall::Running();
    firmware::SlotInfo boot = firmware::FirmwareInstall::BootSelected();
    char line[160];
    snprintf(line, sizeof(line), "运行：%s（%s）", running.label.c_str(),
             running.version.empty() ? "无镜像" : running.version.c_str());
    AddLabel(g_status_box, line);
    snprintf(line, sizeof(line), "下次启动：%s", boot.label.c_str());
    AddLabel(g_status_box, line);
    AddLabel(g_status_box, "分区：");
    for (const auto& slot : firmware::FirmwareInstall::Slots()) {
        if (slot.part == nullptr) {
            snprintf(line, sizeof(line), "  %s：不存在", slot.label.c_str());
        } else {
            snprintf(line, sizeof(line), "  %s：%s  %s  %lu KB", slot.label.c_str(),
                     firmware::FirmwareInstall::StateName(slot.state),
                     slot.version.empty() ? "-" : slot.version.c_str(),
                     static_cast<unsigned long>(slot.size / 1024));
        }
        AddLabel(g_status_box, line);
    }
    uint64_t total = 0, free = 0;
    bool mounted = g_board->storage().IsMounted();
    g_board->storage().GetUsage(total, free);
    if (mounted) {
        snprintf(line, sizeof(line), "SD卡：已挂载，剩余 %.1f GB", free / 1073741824.0);
    } else {
        snprintf(line, sizeof(line), "SD卡：未挂载（需 FAT32）");
    }
    AddLabel(g_status_box, line);
    snprintf(line, sizeof(line), "仓库：%zu 项", g_entries.size());
    AddLabel(g_status_box, line);
    AddButton(g_status_box, "刷新", [](lv_event_t*) { RefreshStatus(); }, nullptr);
}

// ---------------- Repository ----------------

void RebuildRepoList() {
    if (g_repo_list == nullptr) {
        return;
    }
    lv_obj_clean(g_repo_list);
    if (g_entries.empty()) {
        lv_list_add_text(g_repo_list, "仓库为空或无 SD 卡");
        lv_list_add_text(g_repo_list, kRepoRoot);
    }
    for (size_t i = 0; i < g_entries.size(); i++) {
        const auto& entry = g_entries[i];
        char text[200];
        char size_str[32];
        FormatSize(entry.size, size_str, sizeof(size_str));
        if (entry.error.empty()) {
            snprintf(text, sizeof(text), "%s  %s  (%s)", entry.name.c_str(),
                     entry.version.c_str(), size_str);
        } else {
            snprintf(text, sizeof(text), "[无效] %s：%s", entry.channel.c_str(),
                     entry.error.c_str());
        }
        lv_obj_t* btn = lv_list_add_button(g_repo_list, nullptr, text);
        lv_obj_add_event_cb(btn, OnEntryClicked, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<uintptr_t>(i)));
    }
}

void PostScan() {
    // Called from the LVGL task (button) or under lock (init).
    if (g_scan_state.load() == 1) {
        return;
    }
    g_scan_state.store(1);
    if (g_scan_btn != nullptr) {
        lv_obj_add_state(g_scan_btn, LV_STATE_DISABLED);
    }
    Job* job = new Job{JOB_SCAN, firmware::FirmwareEntry(), nullptr};
    if (xQueueSend(g_jobs, &job, 0) != pdTRUE) {
        delete job;
        g_scan_state.store(0);
        if (g_scan_btn != nullptr) {
            lv_obj_remove_state(g_scan_btn, LV_STATE_DISABLED);
        }
    }
}

// ---------------- Install ----------------

void ShowInstallResult(bool ok);

void DoRebootToTarget(lv_event_t* e) {
    if (g_installing) {
        return;
    }
    const esp_partition_t* target =
        static_cast<const esp_partition_t*>(lv_event_get_user_data(e));
    std::string err;
    if (firmware::FirmwareInstall::Switch(target, err)) {
        esp_restart();
    } else {
        ESP_LOGE(TAG, "switch failed: %s", err.c_str());
        lv_obj_t* mbox = lv_msgbox_create(nullptr);
        SetFont(mbox);
        lv_msgbox_add_title(mbox, "切换失败");
        lv_msgbox_add_text(mbox, err.c_str());
        lv_msgbox_add_close_button(mbox);
    }
}

void ShowInstallResult(bool ok) {
    lv_obj_t* mbox = lv_msgbox_create(nullptr);  // modal backdrop
    SetFont(mbox);
    if (ok) {
        lv_msgbox_add_title(mbox, "安装成功");
        char text[200];
        snprintf(text, sizeof(text), "%s %s 已写入 %s。", g_inst_entry.name.c_str(),
                 g_inst_entry.version.c_str(), g_inst_target->label);
        lv_msgbox_add_text(mbox, text);
        lv_obj_t* reboot = lv_msgbox_add_footer_button(mbox, "重启进入");
        lv_obj_add_event_cb(reboot, DoRebootToTarget, LV_EVENT_CLICKED,
                            const_cast<void*>(static_cast<const void*>(g_inst_target)));
        lv_obj_t* later = lv_msgbox_add_footer_button(mbox, "稍后");
        lv_obj_add_event_cb(later, [](lv_event_t* e) {
            lv_obj_delete_async(lv_obj_get_parent((lv_obj_t*)lv_event_get_target(e)));
        }, LV_EVENT_CLICKED, nullptr);
    } else {
        lv_msgbox_add_title(mbox, "安装失败");
        lv_msgbox_add_text(mbox, g_err[0] ? g_err : "未知错误");
        lv_msgbox_add_close_button(mbox);
    }
}

void StartInstall(const firmware::FirmwareEntry& entry, const esp_partition_t* target) {
    if (g_installing || target == nullptr) {
        return;
    }
    g_inst_entry = entry;
    g_inst_target = target;
    g_cancel.store(false);  // reset here (LVGL task), before the worker starts
    g_phase.store(0);
    g_done.store(0);
    g_total.store(entry.size);
    g_installing = true;
    lv_obj_remove_flag(g_progress_panel, LV_OBJ_FLAG_HIDDEN);
    lv_bar_set_value(g_progress_bar, 0, LV_ANIM_OFF);
    lv_label_set_text(g_progress_label, "准备中…");

    Job* job = new Job{JOB_INSTALL, entry, target};
    if (xQueueSend(g_jobs, &job, 0) != pdTRUE) {
        delete job;
        g_installing = false;
        lv_obj_add_flag(g_progress_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

void ConfirmInstallCb(lv_event_t* e) {
    uintptr_t packed = reinterpret_cast<uintptr_t>(lv_event_get_user_data(e));
    int index = static_cast<int>(packed & 0xffff);
    esp_partition_subtype_t subtype = static_cast<esp_partition_subtype_t>(packed >> 16);
    lv_obj_t* mbox = lv_obj_get_parent((lv_obj_t*)lv_event_get_target(e));
    // Snapshot the entry NOW (modal dialog prevents rescan behind it).
    firmware::FirmwareEntry entry;
    if (index >= 0 && index < (int)g_entries.size()) {
        entry = g_entries[index];
    }
    lv_obj_delete_async(mbox);
    if (!entry.error.empty()) {
        return;
    }
    StartInstall(entry, OtaPartition(subtype));
}

void OnEntryClicked(lv_event_t* e) {
    int index = static_cast<int>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
    if (index < 0 || index >= (int)g_entries.size() || g_installing) {
        return;
    }
    const firmware::FirmwareEntry& entry = g_entries[index];
    firmware::SlotInfo running = firmware::FirmwareInstall::Running();

    lv_obj_t* mbox = lv_msgbox_create(nullptr);  // modal
    SetFont(mbox);
    lv_msgbox_add_title(mbox, entry.name.c_str());
    char text[400];
    char size_str[32];
    FormatSize(entry.size, size_str, sizeof(size_str));
    snprintf(text, sizeof(text),
             "版本：%s\n通道：%s\n大小：%s\n目标：%s / %s\nSHA-256：%s\n签名：%s",
             entry.version.c_str(), entry.channel.c_str(), size_str,
             entry.target_chip.empty() ? "?" : entry.target_chip.c_str(),
             entry.target_board.empty() ? "?" : entry.target_board.c_str(),
             entry.sha256.empty() ? "无" : "安装时校验",
             entry.has_signature ? "已签名（安装时验签）" : "未签名");
    if (!entry.error.empty()) {
        snprintf(text + strlen(text), sizeof(text) - strlen(text), "\n错误：%s",
                 entry.error.c_str());
    }
    lv_msgbox_add_text(mbox, text);
    if (entry.error.empty()) {
        lv_obj_t* b0 = lv_msgbox_add_footer_button(mbox, "安装到 ota_0");
        lv_obj_t* b1 = lv_msgbox_add_footer_button(mbox, "安装到 ota_1");
        uintptr_t pack0 = (uintptr_t)index | ((uintptr_t)ESP_PARTITION_SUBTYPE_APP_OTA_0 << 16);
        uintptr_t pack1 = (uintptr_t)index | ((uintptr_t)ESP_PARTITION_SUBTYPE_APP_OTA_1 << 16);
        lv_obj_add_event_cb(b0, ConfirmInstallCb, LV_EVENT_CLICKED, reinterpret_cast<void*>(pack0));
        lv_obj_add_event_cb(b1, ConfirmInstallCb, LV_EVENT_CLICKED, reinterpret_cast<void*>(pack1));
        if (running.label == "ota_0") {
            lv_obj_add_state(b0, LV_STATE_DISABLED);
        } else if (running.label == "ota_1") {
            lv_obj_add_state(b1, LV_STATE_DISABLED);
        }
    }
    lv_msgbox_add_close_button(mbox);
}

// ---------------- Switch ----------------

void DoSwitchCb(lv_event_t* e) {
    if (g_installing) {
        return;
    }
    esp_partition_subtype_t subtype =
        static_cast<esp_partition_subtype_t>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
    const esp_partition_t* target = OtaPartition(subtype);
    lv_obj_t* mbox = lv_obj_get_parent((lv_obj_t*)lv_event_get_target(e));
    lv_obj_delete_async(mbox);
    std::string err;
    if (firmware::FirmwareInstall::Switch(target, err)) {
        esp_restart();
    } else {
        ESP_LOGE(TAG, "switch failed: %s", err.c_str());
        lv_obj_t* errbox = lv_msgbox_create(nullptr);
        SetFont(errbox);
        lv_msgbox_add_title(errbox, "切换失败");
        lv_msgbox_add_text(errbox, err.c_str());
        lv_msgbox_add_close_button(errbox);
    }
}

void ConfirmSwitch(esp_partition_subtype_t subtype, const char* label) {
    if (g_installing) {
        return;
    }
    lv_obj_t* mbox = lv_msgbox_create(nullptr);
    SetFont(mbox);
    lv_msgbox_add_title(mbox, "确认切换");
    char text[128];
    snprintf(text, sizeof(text), "设备将重启进入 %s。继续？", label);
    lv_msgbox_add_text(mbox, text);
    lv_obj_t* ok = lv_msgbox_add_footer_button(mbox, "重启");
    lv_obj_add_event_cb(ok, DoSwitchCb, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(static_cast<uintptr_t>(subtype)));
    lv_msgbox_add_close_button(mbox);
}

void BuildSwitchTab(lv_obj_t* parent) {
    lv_obj_t* box = lv_obj_create(parent);
    lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, 8, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    AddLabel(box, "选择下次启动的固件，设备将立即重启。");
    firmware::SlotInfo running = firmware::FirmwareInstall::Running();
    lv_obj_t* b0 = AddButton(
        box, "启动 ota_0（主）",
        [](lv_event_t*) { ConfirmSwitch(ESP_PARTITION_SUBTYPE_APP_OTA_0, "ota_0"); }, nullptr);
    lv_obj_t* b1 = AddButton(
        box, "启动 ota_1（备）",
        [](lv_event_t*) { ConfirmSwitch(ESP_PARTITION_SUBTYPE_APP_OTA_1, "ota_1"); }, nullptr);
    if (running.label == "ota_0") {
        lv_obj_add_state(b0, LV_STATE_DISABLED);
    } else if (running.label == "ota_1") {
        lv_obj_add_state(b1, LV_STATE_DISABLED);
    }
    AddLabel(box, "两个 OTA 槽都失效时，将回到本救援系统。");
}

// ---------------- Progress overlay (modal: absorbs input) ----------------

void BuildProgressOverlay() {
    g_progress_panel = lv_obj_create(lv_layer_top());
    SetFont(g_progress_panel);
    lv_obj_set_size(g_progress_panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(g_progress_panel, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_progress_panel, LV_OPA_70, 0);
    lv_obj_add_flag(g_progress_panel, LV_OBJ_FLAG_CLICKABLE);  // block input behind
    lv_obj_set_flex_flow(g_progress_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_progress_panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(g_progress_panel, 10, 0);
    lv_obj_add_flag(g_progress_panel, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* card = lv_obj_create(g_progress_panel);
    SetFont(card);
    lv_obj_set_size(card, LV_PCT(85), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_set_style_pad_row(card, 8, 0);
    AddLabel(card, "正在安装固件");
    g_progress_bar = lv_bar_create(card);
    lv_obj_set_width(g_progress_bar, LV_PCT(100));
    lv_bar_set_range(g_progress_bar, 0, 100);
    g_progress_label = AddLabel(card, "准备中…");
    AddButton(card, "取消", [](lv_event_t*) { g_cancel.store(true); }, nullptr);
}

// ---------------- Worker + timer ----------------

void WorkerTask(void*) {
    Job* job = nullptr;
    while (true) {
        if (xQueueReceive(g_jobs, &job, portMAX_DELAY) != pdTRUE || job == nullptr) {
            continue;
        }
        if (job->type == JOB_SCAN) {
            g_board->MountStorage(true);
            auto entries = firmware::FirmwareRepo().Scan(kRepoRoot);
            {
                std::lock_guard<std::mutex> lock(g_scan_mutex);
                g_scan_result = std::move(entries);
            }
            g_scan_state.store(2);
        } else {
            g_phase.store(1);
            g_done.store(0);
            g_total.store(job->entry.size);
            g_err[0] = '\0';
            auto progress = [](uint64_t done, uint64_t total, int phase) -> bool {
                g_done.store(done);
                g_total.store(total);
                g_phase.store(phase);
                return !g_cancel.load();
            };
            std::string err;
            bool ok = firmware::FirmwareInstall::Install(job->entry, job->target, progress, err);
            if (!ok) {
                strncpy(g_err, err.c_str(), sizeof(g_err) - 1);
                g_err[sizeof(g_err) - 1] = '\0';
                g_phase.store(4);
            } else {
                g_phase.store(3);
            }
        }
        delete job;
    }
}

void TimerCb(lv_timer_t*) {
    if (g_scan_state.load() == 2) {
        {
            std::lock_guard<std::mutex> lock(g_scan_mutex);
            g_entries = std::move(g_scan_result);
        }
        g_scan_state.store(0);
        RebuildRepoList();
        RefreshStatus();
        if (g_scan_btn != nullptr) {
            lv_obj_remove_state(g_scan_btn, LV_STATE_DISABLED);
        }
    }
    if (g_installing) {
        int phase = g_phase.load();
        char line[160];
        if (phase == 1) {
            lv_label_set_text(g_progress_label, "校验 SHA-256…");
        } else if (phase == 2) {
            uint64_t done = g_done.load(), total = g_total.load();
            int pct = total > 0 ? static_cast<int>(done * 100 / total) : 0;
            lv_bar_set_value(g_progress_bar, pct, LV_ANIM_OFF);
            snprintf(line, sizeof(line), "写入 %llu / %llu KB",
                     static_cast<unsigned long long>(done / 1024),
                     static_cast<unsigned long long>(total / 1024));
            lv_label_set_text(g_progress_label, line);
        } else if (phase == 3 || phase == 4) {
            g_installing = false;
            lv_obj_add_flag(g_progress_panel, LV_OBJ_FLAG_HIDDEN);
            ShowInstallResult(phase == 3);
        }
    }
}

}  // namespace

void rescue_ui_run(RescueBoard* board) {
    g_board = board;

    // Build the whole UI under the LVGL lock (main task; LVGL task already runs).
    lvgl_port_lock(0);
    lv_obj_t* screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, g_font, 0);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101418), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(0xe8eaed), 0);

    lv_obj_t* tv = lv_tabview_create(screen);
    lv_tabview_set_tab_bar_position(tv, LV_DIR_TOP);
    lv_tabview_set_tab_bar_size(tv, 40);
    lv_obj_set_size(tv, LV_PCT(100), LV_PCT(100));
    SetFont(tv);

    lv_obj_t* tab_status = lv_tabview_add_tab(tv, "状态");
    lv_obj_t* tab_repo = lv_tabview_add_tab(tv, "仓库");
    lv_obj_t* tab_switch = lv_tabview_add_tab(tv, "切换");

    g_status_box = lv_obj_create(tab_status);
    lv_obj_set_size(g_status_box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g_status_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(g_status_box, 6, 0);
    lv_obj_remove_flag(g_status_box, LV_OBJ_FLAG_SCROLLABLE);

    g_repo_list = lv_list_create(tab_repo);
    lv_obj_set_size(g_repo_list, LV_PCT(100), LV_PCT(82));
    g_scan_btn = AddButton(tab_repo, "扫描仓库", [](lv_event_t*) { PostScan(); }, nullptr);
    lv_obj_set_width(g_scan_btn, LV_PCT(100));

    BuildSwitchTab(tab_switch);
    BuildProgressOverlay();
    lv_timer_create(TimerCb, 100, nullptr);
    RefreshStatus();
    lvgl_port_unlock();

    // Single persistent worker owns all SD mounting/scanning/installing.
    g_jobs = xQueueCreate(4, sizeof(Job*));
    if (g_jobs == nullptr) {
        ESP_LOGE(TAG, "job queue create failed");
        return;
    }
    if (xTaskCreate(WorkerTask, "rescue_work", 16384, nullptr, 3, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "worker task create failed");
        return;
    }
    lvgl_port_lock(0);
    PostScan();
    lvgl_port_unlock();
    ESP_LOGI(TAG, "rescue UI ready (touch=%s)", board->touch_ready() ? "yes" : "no");
}

#endif  // CONFIG_METAOS_RESCUE_MODE
