#include "metaos_reminders.h"

#include <esp_log.h>
#include <cJSON.h>

#include <algorithm>
#include <cstdio>

#include "cjson_utils.h"

#define TAG "MetaOS"

namespace metaos {

namespace {
constexpr const char* kRemindersFile = "reminders.json";
constexpr size_t kMaxReminders = 20;
constexpr size_t kMaxMessageBytes = 200;
constexpr size_t kMaxFileBytes = 16 * 1024;
constexpr time_t kSecondsPerDay = 24 * 60 * 60;

bool EarlierFirst(const Reminder& a, const Reminder& b) { return a.due < b.due; }

ToolResult JsonResult(cJSON* json) {
    if (json == nullptr) {
        return std::unexpected("Out of memory");
    }
    return ReturnValue(std::in_place_type<cJSON*>, json);
}

// Parses "HH:MM" (next occurrence) or "YYYY-MM-DD HH:MM". The clock holds local time.
bool ParseTime(const std::string& text, time_t now, time_t& due) {
    int year = 0, month = 0, day = 0, hour = 0, minute = 0;
    char extra = 0;
    bool date_given = false;
    struct tm local = {};
    localtime_r(&now, &local);
    if (sscanf(text.c_str(), "%d-%d-%d %d:%d%c", &year, &month, &day, &hour, &minute, &extra) ==
        5) {
        date_given = true;
        if (month < 1 || month > 12 || day < 1 || day > 31) {
            return false;
        }
        local.tm_year = year - 1900;
        local.tm_mon = month - 1;
        local.tm_mday = day;
    } else if (sscanf(text.c_str(), "%d:%d%c", &hour, &minute, &extra) != 2) {
        return false;
    }
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        return false;
    }
    local.tm_hour = hour;
    local.tm_min = minute;
    local.tm_sec = 0;
    local.tm_isdst = -1;
    due = mktime(&local);
    if (due == static_cast<time_t>(-1)) {
        return false;
    }
    if (!date_given && due <= now) {
        due += kSecondsPerDay;
    }
    return true;
}

cJSON* ReminderToJson(const Reminder& reminder) {
    cJSON* item = cJSON_CreateObject();
    if (item == nullptr) {
        return nullptr;
    }
    cJSON_AddNumberToObject(item, "id", reminder.id);
    cJSON_AddStringToObject(item, "time", FormatTime(reminder.due, "%Y-%m-%d %H:%M").c_str());
    cJSON_AddStringToObject(item, "message", reminder.message.c_str());
    cJSON_AddBoolToObject(item, "daily", reminder.daily);
    return item;
}
}  // namespace

void ReminderStore::Load() {
    if (!storage_.IsMounted()) {
        return;
    }
    std::string text;
    if (!ReadFile(storage_.DataPath(kRemindersFile), text, kMaxFileBytes)) {
        return;
    }
    CJsonUniquePtr root(cJSON_ParseWithLength(text.data(), text.size()));
    if (!cJSON_IsObject(root.get())) {
        ESP_LOGW(TAG, "Ignoring invalid %s", kRemindersFile);
        return;
    }
    auto next_id = cJSON_GetObjectItem(root.get(), "next_id");
    if (cJSON_IsNumber(next_id) && next_id->valueint > 0) {
        next_id_ = next_id->valueint;
    }
    auto list = cJSON_GetObjectItem(root.get(), "reminders");
    if (!cJSON_IsArray(list)) {
        return;
    }
    reminders_.clear();
    cJSON* item = nullptr;
    cJSON_ArrayForEach (item, list) {
        auto id = cJSON_GetObjectItem(item, "id");
        auto due = cJSON_GetObjectItem(item, "due");
        auto message = cJSON_GetObjectItem(item, "message");
        auto daily = cJSON_GetObjectItem(item, "daily");
        if (!cJSON_IsNumber(id) || id->valueint <= 0 || !cJSON_IsNumber(due) ||
            due->valuedouble <= 0 || !cJSON_IsString(message) ||
            reminders_.size() >= kMaxReminders) {
            continue;
        }
        Reminder reminder;
        reminder.id = id->valueint;
        reminder.due = static_cast<time_t>(due->valuedouble);
        reminder.message = Utf8Truncate(message->valuestring, kMaxMessageBytes);
        reminder.daily = cJSON_IsTrue(daily);
        next_id_ = std::max(next_id_, reminder.id + 1);
        reminders_.push_back(std::move(reminder));
    }
    std::sort(reminders_.begin(), reminders_.end(), EarlierFirst);
    ++revision_;
    ESP_LOGI(TAG, "Loaded %u reminder(s)", static_cast<unsigned>(reminders_.size()));
}

void ReminderStore::Insert(Reminder reminder) {
    ++revision_;
    reminders_.push_back(std::move(reminder));
    std::sort(reminders_.begin(), reminders_.end(), EarlierFirst);
}

bool ReminderStore::Save() {
    persisted_ = false;
    if (!storage_.Ensure()) {
        return false;
    }
    CJsonUniquePtr root(cJSON_CreateObject());
    if (root == nullptr) {
        return false;
    }
    cJSON_AddNumberToObject(root.get(), "next_id", next_id_);
    cJSON* list = cJSON_AddArrayToObject(root.get(), "reminders");
    if (list == nullptr) {
        return false;
    }
    for (const auto& reminder : reminders_) {
        cJSON* item = cJSON_CreateObject();
        if (item == nullptr) {
            return false;
        }
        cJSON_AddNumberToObject(item, "id", reminder.id);
        cJSON_AddNumberToObject(item, "due", static_cast<double>(reminder.due));
        cJSON_AddStringToObject(item, "message", reminder.message.c_str());
        cJSON_AddBoolToObject(item, "daily", reminder.daily);
        cJSON_AddItemToArray(list, item);
    }
    CJsonStringUniquePtr text(cJSON_Print(root.get()));
    persisted_ =
        text != nullptr && WriteFileReplacing(storage_.DataPath(kRemindersFile), text.get());
    return persisted_;
}

ToolResult ReminderStore::Add(const std::string& message, int in_minutes, const std::string& at,
                              bool daily) {
    if (message.empty()) {
        return std::unexpected("The reminder message is empty");
    }
    if ((in_minutes > 0) == !at.empty()) {
        return std::unexpected("Set exactly one of in_minutes or at");
    }
    if (!IsClockValid()) {
        return std::unexpected("The clock is not synchronized yet; try again in a minute");
    }
    if (reminders_.size() >= kMaxReminders) {
        return std::unexpected("Too many reminders (20). Cancel some first.");
    }

    time_t now = time(nullptr);
    Reminder reminder;
    if (in_minutes > 0) {
        reminder.due = now + static_cast<time_t>(in_minutes) * 60;
    } else if (!ParseTime(at, now, reminder.due)) {
        return std::unexpected("Invalid time; use \"HH:MM\" or \"YYYY-MM-DD HH:MM\"");
    }
    if (reminder.due <= now) {
        return std::unexpected("That time is in the past");
    }
    reminder.id = next_id_++;
    reminder.message = Utf8Truncate(message, kMaxMessageBytes);
    reminder.daily = daily;
    Insert(reminder);

    cJSON* json = ReminderToJson(reminder);
    if (json != nullptr && !Save()) {
        cJSON_AddStringToObject(json, "warning",
                                "Not saved to the SD card; it will be lost on restart");
    }
    return JsonResult(json);
}

ToolResult ReminderStore::List() const {
    cJSON* list = cJSON_CreateArray();
    if (list == nullptr) {
        return std::unexpected("Out of memory");
    }
    for (const auto& reminder : reminders_) {
        cJSON* item = ReminderToJson(reminder);
        if (item != nullptr) {
            cJSON_AddItemToArray(list, item);
        }
    }
    return JsonResult(list);
}

ToolResult ReminderStore::Cancel(int id) {
    auto it = std::find_if(reminders_.begin(), reminders_.end(),
                           [id](const Reminder& reminder) { return reminder.id == id; });
    if (it == reminders_.end()) {
        return std::unexpected("No reminder with id " + std::to_string(id));
    }
    reminders_.erase(it);
    ++revision_;
    Save();
    return std::string("Cancelled reminder " + std::to_string(id));
}

std::vector<Reminder> ReminderStore::TakeDue(time_t now) {
    std::vector<Reminder> due;
    for (auto it = reminders_.begin(); it != reminders_.end();) {
        if (it->due > now) {
            ++it;
            continue;
        }
        due.push_back(*it);
        if (it->daily) {
            while (it->due <= now) {
                it->due += kSecondsPerDay;
            }
            ++it;
        } else {
            it = reminders_.erase(it);
        }
    }
    if (!due.empty()) {
        std::sort(reminders_.begin(), reminders_.end(), EarlierFirst);
        ++revision_;
        Save();
    }
    return due;
}

bool ReminderStore::Snooze(const Reminder& reminder, int minutes) {
    if (minutes <= 0 || reminders_.size() >= kMaxReminders) {
        return false;
    }
    Reminder copy;
    copy.id = next_id_++;
    copy.due = time(nullptr) + static_cast<time_t>(minutes) * 60;
    copy.message = reminder.message;
    copy.daily = false;
    Insert(std::move(copy));
    Save();
    return true;
}

}  // namespace metaos
