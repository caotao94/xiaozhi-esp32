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
inline constexpr const char* kHermesNotConfigured = "未配置：在 TF 卡创建 metaos/hermes.json";
inline constexpr const char* kHermesHint = "对小智说：让 Hermes 帮我查一下……";
inline constexpr const char* kHermesIdle = "还没有任务";
inline constexpr const char* kRunning = "运行中";
inline constexpr const char* kDone = "已完成";
inline constexpr const char* kFailed = "失败";
inline constexpr const char* kSteps = "步";
inline constexpr const char* kStop = "停止";
inline constexpr const char* kRead = "朗读";
inline constexpr const char* kClose = "关闭";
inline constexpr const char* kTruncated = "（内容过长，完整结果已存到 TF 卡）";
inline constexpr const char* kHermesDone = "Hermes 完成了任务";
inline constexpr const char* kHermesFailed = "Hermes 任务失败";
inline constexpr const char* kStorage = "TF 卡";
inline constexpr const char* kUsed = "已用";
inline constexpr const char* kRetry = "重新检测";
inline constexpr const char* kSpeakReminders = "提醒语音播报";
inline constexpr const char* kSpeakHermes = "Hermes 完成后语音播报";
inline constexpr const char* kBusy = "正在对话，稍后再试";
inline constexpr const char* kVoiceHint = "长按 BOOT 键打开或关闭此界面";
// Prompts sent to the XiaoZhi server as text input. The assistant answers them aloud.
inline constexpr const char* kSpeakReminderPrompt = "提醒时间到了，请用一句话提醒我：";
inline constexpr const char* kSpeakHermesPrompt =
    "Hermes 已经完成任务。请调用 self.hermes.get_status 读取结果，用两三句话告诉我。";
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
inline constexpr const char* kHermesNotConfigured = "Not set up: create metaos/hermes.json";
inline constexpr const char* kHermesHint = "Say: ask Hermes to look up ...";
inline constexpr const char* kHermesIdle = "No task yet";
inline constexpr const char* kRunning = "Running";
inline constexpr const char* kDone = "Done";
inline constexpr const char* kFailed = "Failed";
inline constexpr const char* kSteps = "steps";
inline constexpr const char* kStop = "Stop";
inline constexpr const char* kRead = "Read";
inline constexpr const char* kClose = "Close";
inline constexpr const char* kTruncated = "(Too long; the full result is on the card)";
inline constexpr const char* kHermesDone = "Hermes finished";
inline constexpr const char* kHermesFailed = "Hermes failed";
inline constexpr const char* kStorage = "TF card";
inline constexpr const char* kUsed = "used";
inline constexpr const char* kRetry = "Retry";
inline constexpr const char* kSpeakReminders = "Speak reminders";
inline constexpr const char* kSpeakHermes = "Speak Hermes results";
inline constexpr const char* kBusy = "Busy, try again later";
inline constexpr const char* kVoiceHint = "Long press BOOT to open or close";
inline constexpr const char* kSpeakReminderPrompt =
    "A reminder is due. Remind me in one short sentence: ";
inline constexpr const char* kSpeakHermesPrompt =
    "Hermes has finished its task. Call self.hermes.get_status to read the result and tell me "
    "the key points in two or three sentences.";
#endif

}  // namespace metaos::text

#endif  // METAOS_STRINGS_H
