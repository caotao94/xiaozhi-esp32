#ifndef METAOS_REMINDERS_H
#define METAOS_REMINDERS_H

#include <ctime>
#include <string>
#include <vector>

#include "mcp_server.h"
#include "metaos_common.h"

namespace metaos {

struct Reminder {
    int id = 0;
    time_t due = 0;
    std::string message;
    bool daily = false;
};

// Scheduled reminders, saved to reminders.json on the SD card. They work without a card too,
// but are then lost on restart. Main task only.
class ReminderStore {
public:
    explicit ReminderStore(Storage& storage) : storage_(storage) {}

    void Load();
    // Exactly one of in_minutes (> 0) or at ("HH:MM" or "YYYY-MM-DD HH:MM") must be set.
    ToolResult Add(const std::string& message, int in_minutes, const std::string& at, bool daily);
    ToolResult List() const;
    ToolResult Cancel(int id);

    // Removes reminders that are due (daily ones move to their next day) and returns them.
    std::vector<Reminder> TakeDue(time_t now);
    // Schedules a one-off copy of `reminder` `minutes` from now (the popup "snooze" button).
    bool Snooze(const Reminder& reminder, int minutes);
    const std::vector<Reminder>& items() const { return reminders_; }
    size_t size() const { return reminders_.size(); }
    // False when the last change could not be written to the SD card.
    bool persisted() const { return persisted_; }
    // Changes whenever the list changes.
    unsigned revision() const { return revision_; }

private:
    bool Save();
    void Insert(Reminder reminder);

    Storage& storage_;
    std::vector<Reminder> reminders_;
    int next_id_ = 1;
    bool persisted_ = true;
    unsigned revision_ = 0;
};

}  // namespace metaos

#endif  // METAOS_REMINDERS_H
