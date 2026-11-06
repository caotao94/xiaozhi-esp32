#ifndef METAOS_WORKER_H
#define METAOS_WORKER_H

#include <functional>

namespace metaos {

// Runs slow MetaOS jobs (image decoding, speech synthesis) off the main task, one at a time,
// on a single low-priority task with a PSRAM stack. Jobs must not touch LVGL or MetaOS state
// directly; they hand results back with Application::Schedule().
//
// Main task. Returns false when the queue is full or the worker could not be started.
bool PostWork(std::function<void()> job);

}  // namespace metaos

#endif  // METAOS_WORKER_H
