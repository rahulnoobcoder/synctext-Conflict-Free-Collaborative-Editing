#pragma once

#include <thread>

#include "editor.h"

namespace syntext {

class ThreadManager {
 public:
  explicit ThreadManager(Editor& editor);

  void start();
  void stop();

 private:
  Editor& editor_;
  std::thread network_thread_;
  std::thread merge_thread_;
};

}  // namespace syntext
