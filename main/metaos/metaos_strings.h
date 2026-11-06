#ifndef METAOS_STRINGS_H
#define METAOS_STRINGS_H

#include <sdkconfig.h>

// MetaOS UI text. Chinese builds use the Chinese strings, every other language uses English.
// Every Chinese character here is in the font's "common" charset (checked against
// managed_components/78__xiaozhi-fonts/charsets/common.json).
namespace metaos::text {

#if CONFIG_LANGUAGE_ZH_CN || CONFIG_LANGUAGE_ZH_TW
inline constexpr const char* kMusic = "音乐";
inline constexpr const char* kReminders = "提醒";
inline constexpr const char* kSettings = "设置";
inline constexpr const char* kNoCard = "未检测到 TF 卡";
inline constexpr const char* kNeedFat32 = "TF 卡不是 FAT32 格式，请在电脑上格式化";
inline constexpr const char* kNoMusicFolder = "无法打开 music 文件夹";
inline constexpr const char* kNoSongs = "把 .ogg 音乐放进 TF 卡的 music 文件夹";
inline constexpr const char* kSongs = "首";
inline constexpr const char* kNotPlaying = "未在播放";
inline constexpr const char* kWaitingForChat = "对话结束后开始播放";
inline constexpr const char* kPlayFailed = "播放失败";
inline constexpr const char* kPrevPage = "上一页";
inline constexpr const char* kNextPage = "下一页";
inline constexpr const char* kShuffle = "随机";
inline constexpr const char* kNoReminders = "还没有提醒";
inline constexpr const char* kReminderHint = "对小智说：十分钟后提醒我关火";
inline constexpr const char* kDaily = "每天";
inline constexpr const char* kCancel = "取消";
inline constexpr const char* kCancelled = "已取消";
inline constexpr const char* kNotSaved = "未保存到 TF 卡，重启后会丢失";
inline constexpr const char* kReminderDue = "提醒";
inline constexpr const char* kSnooze = "10 分钟后";
inline constexpr const char* kGotIt = "知道了";
inline constexpr const char* kSnoozed = "10 分钟后再提醒";
inline constexpr const char* kStorage = "TF 卡";
inline constexpr const char* kRetry = "重新检测";
inline constexpr const char* kVoiceHint = "长按 BOOT 键打开或关闭此界面";
#else
inline constexpr const char* kMusic = "Music";
inline constexpr const char* kReminders = "Reminders";
inline constexpr const char* kSettings = "Settings";
inline constexpr const char* kNoCard = "No TF card";
inline constexpr const char* kNeedFat32 = "The card is not FAT32. Format it on a PC";
inline constexpr const char* kNoMusicFolder = "Cannot open the music folder";
inline constexpr const char* kNoSongs = "Copy .ogg files to the music folder";
inline constexpr const char* kSongs = "songs";
inline constexpr const char* kNotPlaying = "Not playing";
inline constexpr const char* kWaitingForChat = "Starts after the conversation";
inline constexpr const char* kPlayFailed = "Playback failed";
inline constexpr const char* kPrevPage = "Prev";
inline constexpr const char* kNextPage = "Next";
inline constexpr const char* kShuffle = "Shuffle";
inline constexpr const char* kNoReminders = "No reminders";
inline constexpr const char* kReminderHint = "Say: remind me in ten minutes";
inline constexpr const char* kDaily = "Daily";
inline constexpr const char* kCancel = "Cancel";
inline constexpr const char* kCancelled = "Cancelled";
inline constexpr const char* kNotSaved = "Not saved to the card";
inline constexpr const char* kReminderDue = "Reminder";
inline constexpr const char* kSnooze = "In 10 min";
inline constexpr const char* kGotIt = "OK";
inline constexpr const char* kSnoozed = "Snoozed for 10 minutes";
inline constexpr const char* kStorage = "TF card";
inline constexpr const char* kRetry = "Retry";
inline constexpr const char* kVoiceHint = "Long press BOOT to open or close";
#endif

}  // namespace metaos::text

#endif  // METAOS_STRINGS_H
