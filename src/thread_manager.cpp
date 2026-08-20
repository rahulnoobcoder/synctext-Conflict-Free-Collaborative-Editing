#include "thread_manager.h"

namespace syntext {

ThreadManager::ThreadManager(Editor& editor) : editor_(editor) {}

void ThreadManager::start() {
  network_thread_ = std::thread([this]() { editor_.network().run(editor_.running()); });
  merge_thread_ = std::thread([this]() { editor_.runMergeLoop(); });
}

void ThreadManager::stop() {
  editor_.running().store(false, std::memory_order_release);
  if (network_thread_.joinable()) {
    network_thread_.join();
  }
  if (merge_thread_.joinable()) {
    merge_thread_.join();
  }
}

}  // namespace syntext
