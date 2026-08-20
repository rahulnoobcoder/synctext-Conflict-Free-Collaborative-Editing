#include <atomic>
#include <csignal>
#include <iostream>
#include <string>

#include "editor.h"
#include "thread_manager.h"

namespace {
std::atomic<bool>* g_running = nullptr;

void handleSignal(int) {
  if (g_running) {
    g_running->store(false, std::memory_order_release);
  }
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "Usage: ./editor <user_id> [port] [--headless]\n";
    return 1;
  }

  std::string user_id = argv[1];
  uint16_t port = 9000 + static_cast<uint16_t>(std::hash<std::string>{}(user_id) % 1000);
  bool headless = false;

  for (int i = 2; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--headless") {
      headless = true;
    } else {
      try { port = static_cast<uint16_t>(std::stoi(arg)); } catch (...) {}
    }
  }

  syntext::Editor editor(user_id, port, headless);
  syntext::ThreadManager threads(editor);
  g_running = &editor.running();
  std::signal(SIGINT,  handleSignal);
  std::signal(SIGTERM, handleSignal);

  threads.start();
  editor.runMainLoop();
  threads.stop();

  return 0;
}
