#ifndef METAOS_WORKER_H
#define METAOS_WORKER_H

#include <functional>

namespace metaos {

// Runs slow MetaOS jobs (image decoding, speech synthesis) off the main task, one at a time,
// on a single low-priority task with a PSRAM stack. Jobs must not touch LVGL or MetaOS state
// directly; they hand results back with Application::Schedule().
//
// Main task. Returns false when the queue is full, the worker could not be started, or work
// is disabled (see SetWorkEnabled).
bool PostWork(std::function<void()> job);

// Main task. Gates new jobs: while disabled, PostWork returns false. Used to quiesce file
// access before handing the SD card to USB MSC. Jobs already queued/running still finish.
void SetWorkEnabled(bool enabled);

// Any task. True when no job is queued or running. Used to wait for the worker to drain
// before switching SD-card ownership to USB.
bool WorkIdle();

}  // namespace metaos

#endif  // METAOS_WORKER_H
