#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "crdt.h"
#include "network.h"
#include "ui.h"
#include "utils.h"

namespace syntext {

class Editor {
 public:
  Editor(const std::string& user_id, uint16_t port, bool headless = false);

  // Main UI loop (ncurses) or headless stdin loop.
  void runMainLoop();
  // Network-aware merge thread (applies remote ops, updates doc+UI).
  void runMergeLoop();

  std::atomic<bool>& running();
  Network& network();

 private:
  std::string user_id_;
  bool        headless_;
  LamportClock clock_;
  uint64_t    op_seq_  = 1;
  std::string base_content_;

  std::vector<Operation>          op_log_;
  std::unordered_set<std::string> op_set_;

  std::atomic<bool> running_;

  // Document protected by doc_mutex_ — read by main thread (render),
  // written by merge thread (remote ops) and main thread (local ops).
  std::mutex                       doc_mutex_;
  CrdtDocument                     document_;

  // Queues between threads — unchanged from before.
  SpscQueue<Operation, 16384>  local_ops_;
  SpscQueue<Operation, 16384>  inbound_ops_;
  SpscQueue<std::string, 4096> outbound_msgs_;
  SpscQueue<uint64_t, 2048>    merge_hashes_;
  SpscQueue<std::string, 256>  new_peer_syncs_;

  // Inbound cursor messages (network → main thread).
  SpscQueue<CursorMessage, 512> inbound_cursors_;

  Ui      ui_;
  Network network_;

  int last_peer_count_ = 0;

  std::string makeDocPath() const;
  void ensureDocFile();
  std::string wrapOpsMessage(const Message& msg) const;
  void sendCursorBroadcast(const std::string& cursor_json);
};

}  // namespace syntext
