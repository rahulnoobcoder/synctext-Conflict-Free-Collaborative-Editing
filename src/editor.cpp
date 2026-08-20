#include "editor.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>

namespace syntext {

namespace {
const int kMergePollMs  = 50;   // merge thread tick
const int kMainPollMs   = 16;   // UI frame tick (~60fps)
const size_t kBatchSize      = 5;
const uint64_t kBatchFlushMs = 50;  // tuned down for char-level ops

template <typename Queue, typename Item>
void pushWithRetry(Queue& queue, const Item& item) {
  for (int i = 0; i < 5000; ++i) {
    if (queue.push(item)) return;
    std::this_thread::yield();
  }
  // Silently drop if full — the network thread will handle backpressure.
}
}  // namespace

Editor::Editor(const std::string& user_id, uint16_t port, bool headless)
    : user_id_(user_id),
      headless_(headless),
      running_(true),
      network_(user_id, port, outbound_msgs_, inbound_ops_, new_peer_syncs_, inbound_cursors_) {
  // Load any pre-existing doc file as base content.
  std::string doc_path = makeDocPath();
  std::string initial;
  if (readFile(doc_path, initial)) {
    base_content_ = initial;
  }
  std::lock_guard<std::mutex> lk(doc_mutex_);
  document_.loadFromString(base_content_);
}

std::string Editor::makeDocPath() const {
  return user_id_ + "_doc.txt";
}

void Editor::ensureDocFile() {
  std::string content;
  if (!readFile(makeDocPath(), content)) {
    writeFileAtomic(makeDocPath(), "");
  }
}

std::string Editor::wrapOpsMessage(const Message& msg) const {
  std::string payload = serializeMessage(msg);
  // Insert type field after opening brace.
  payload.insert(1, "\"type\":\"ops\",");
  return payload;
}

void Editor::sendCursorBroadcast(const std::string& cursor_json) {
  // Cursor broadcasts bypass the batch queue — push straight to outbound.
  // They are small and low-latency; we use best-effort (no retry).
  outbound_msgs_.push(cursor_json);
}

// ---------------------------------------------------------------------------
// runMainLoop — ncurses UI loop (main thread).
// ---------------------------------------------------------------------------
void Editor::runMainLoop() {
  if (headless_) {
      // Headless mode: read InsertChar ops from stdin in the format:
      //   I <char>
      //   D          (delete char before cursor)
      // Used by test_headless.sh for CI stress testing.
      CharID cursor_anchor = CharID::head();
      int cursor_fallback = 0;
    std::string line;
    while (running_.load(std::memory_order_acquire) && std::getline(std::cin, line)) {
      if (line.empty()) continue;
      std::lock_guard<std::mutex> lk(doc_mutex_);
      
      int cursor_pos = 0;
      if (cursor_anchor.is_head()) {
        cursor_pos = 0;
      } else {
        int p = document_.positionOfCharID(cursor_anchor);
        if (p >= 0) {
          cursor_pos = p + 1;
        } else {
          cursor_pos = std::min(cursor_fallback, document_.visibleCharCount());
        }
      }

      if (line[0] == 'I' && line.size() >= 3) {
        char ch = line[2];
        CharID after = (cursor_pos > 0) ? document_.visibleCharID(cursor_pos - 1)
                                        : CharID::head();
        CharID new_id = {user_id_, op_seq_};
        Operation op;
        op.op_id      = user_id_ + ":" + std::to_string(op_seq_++);
        op.user_id    = user_id_;
        op.timestamp  = clock_.tick();
        op.type       = OpType::InsertChar;
        op.after_id   = after;
        op.new_char_id = new_id;
        op.target_id  = CharID::head();
        op.ch         = ch;
        document_.applyOperation(op);
        pushWithRetry(local_ops_, op);
        ++cursor_pos;
      } else if (line[0] == 'D' && cursor_pos > 0) {
        CharID target = document_.visibleCharID(cursor_pos - 1);
        Operation op;
        op.op_id     = user_id_ + ":" + std::to_string(op_seq_++);
        op.user_id   = user_id_;
        op.timestamp = clock_.tick();
        op.type      = OpType::DeleteChar;
        op.target_id = target;
        op.after_id  = CharID::head();
        op.new_char_id = CharID::head();
        op.ch        = 0;
        document_.applyOperation(op);
        pushWithRetry(local_ops_, op);
        --cursor_pos;
      } else if (line[0] == 'M' && line.size() >= 3) { // Move cursor forward
        int amt = std::stoi(line.substr(2));
        cursor_pos = std::min(cursor_pos + amt, document_.visibleCharCount());
      } else if (line[0] == 'P') {
        // Print current visible content (for test assertions).
        std::cout << document_.toString() << std::flush;
      }

      cursor_anchor = (cursor_pos > 0) ? document_.visibleCharID(cursor_pos - 1) : CharID::head();
      cursor_fallback = cursor_pos;

      // Flush batch if needed (simplified – just always push for headless).
      if (!local_ops_.empty()) {
        // Build batch message.
        std::vector<Operation> batch;
        Operation o;
        while (batch.size() < kBatchSize && local_ops_.pop(o)) {
          batch.push_back(o);
        }
        if (!batch.empty()) {
          Message msg;
          msg.sender_id = user_id_;
          msg.ops = batch;
          pushWithRetry(outbound_msgs_, wrapOpsMessage(msg));
        }
      }
    }
    running_.store(false, std::memory_order_release);
    return;
  }

  // --- ncurses mode ---
  if (!ui_.init()) {
    std::cerr << "Failed to init ncurses\n";
    running_.store(false);
    return;
  }

  std::vector<Operation> pending_batch;
  uint64_t last_flush = nowMillis();

  // Initial render.
  {
    std::lock_guard<std::mutex> lk(doc_mutex_);
    ui_.render(document_, user_id_);
  }

  while (running_.load(std::memory_order_acquire)) {
    // Drain incoming cursor messages from the network.
    {
      CursorMessage cm;
      while (inbound_cursors_.pop(cm)) {
        ui_.updatePeerCursor(cm.user_id, cm.after_id);
      }
    }

    int key = getch();
    if (key != ERR) {
      KeyResult kr;
      {
        std::lock_guard<std::mutex> lk(doc_mutex_);
        kr = ui_.handleKey(key, document_, user_id_, clock_, op_seq_);
      }

      if (kr.quit) {
        running_.store(false, std::memory_order_release);
        break;
      }

      // Push local ops to merge thread.
      for (const auto& op : kr.ops) {
        pushWithRetry(local_ops_, op);
        pending_batch.push_back(op);
      }

      // Flush pending ops *before* broadcasting the cursor that may reference
      // them.  Both messages go through the same outbound queue → same TCP
      // stream, so this guarantees wire-order: peers always learn about a new
      // character before (or at worst in the same delivery) as the cursor
      // message that points at it.
      if (!pending_batch.empty()) {
        Message msg;
        msg.sender_id = user_id_;
        msg.ops = pending_batch;
        pushWithRetry(outbound_msgs_, wrapOpsMessage(msg));
        pending_batch.clear();
        last_flush = nowMillis();
      }

      // Cursor broadcast — now sent after the ops it references.
      if (!kr.cursor_json.empty()) {
        sendCursorBroadcast(kr.cursor_json);
      }
    }

    // Flush batch to network.
    uint64_t now = nowMillis();
    if (!pending_batch.empty() &&
        (pending_batch.size() >= kBatchSize || now - last_flush >= kBatchFlushMs)) {
      Message msg;
      msg.sender_id = user_id_;
      msg.ops = pending_batch;
      pushWithRetry(outbound_msgs_, wrapOpsMessage(msg));
      pending_batch.clear();
      last_flush = now;
    }

    // Redraw (merge thread sets dirty flag via merge_hashes_).
    {
      uint64_t dummy = 0;
      bool got_remote = false;
      while (merge_hashes_.pop(dummy)) { got_remote = true; }
      if (got_remote || key != ERR) {
        std::lock_guard<std::mutex> lk(doc_mutex_);
        ui_.render(document_, user_id_);
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(kMainPollMs));
  }

  ui_.teardown();
}

// ---------------------------------------------------------------------------
// runMergeLoop — merge thread.
// Applies remote ops to document_, writes snapshot file, signals UI dirty.
// ---------------------------------------------------------------------------
void Editor::runMergeLoop() {
  int last_peer_count_ = 0;

  while (running_.load(std::memory_order_acquire)) {
    std::vector<Operation> ops;
    Operation op;
    while (local_ops_.pop(op))   ops.push_back(op);
    while (inbound_ops_.pop(op)) {
      clock_.update(op.timestamp);
      ops.push_back(op);
    }

    if (!ops.empty()) {
      bool changed = false;
      for (const auto& entry : ops) {
        if (op_set_.insert(entry.op_id).second) {
          op_log_.push_back(entry);
          changed = true;
        }
      }

      if (changed) {
        std::sort(op_log_.begin(), op_log_.end(),
                  [](const Operation& a, const Operation& b) {
                    if (a.timestamp != b.timestamp) return a.timestamp < b.timestamp;
                    if (a.user_id   != b.user_id)   return a.user_id   < b.user_id;
                    return a.op_id < b.op_id;
                  });

        // Replay all ops from base into a fresh document.
        {
          std::lock_guard<std::mutex> lk(doc_mutex_);
          document_.loadFromString(base_content_);
          document_.applyOperations(op_log_);
        }

        // Write snapshot file (used for late-join state sync).
        {
          std::lock_guard<std::mutex> lk(doc_mutex_);
          std::string content = document_.toString();
          writeFileAtomic(makeDocPath(), content);
          merge_hashes_.push(fnv1aHash(content));
        }

        // Push full op-log snapshot for newly connected peers.
        Message snap;
        snap.sender_id = user_id_;
        snap.ops       = op_log_;
        new_peer_syncs_.push(wrapOpsMessage(snap));

        if (headless_) {
          std::lock_guard<std::mutex> lk(doc_mutex_);
          std::string content = document_.toString();
          // In headless mode, print to stderr for monitoring.
          std::cerr << "[merge] doc=" << content << "\n";
        }
      }
    }

    // Snapshot push for idle peers.
    int peers_seen = network_.new_peer_count_.load(std::memory_order_acquire);
    if (peers_seen > last_peer_count_) {
      last_peer_count_ = peers_seen;
      if (!op_log_.empty()) {
        Message snap;
        snap.sender_id = user_id_;
        snap.ops       = op_log_;
        new_peer_syncs_.push(wrapOpsMessage(snap));
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(kMergePollMs));
  }
}

std::atomic<bool>& Editor::running() { return running_; }
Network& Editor::network()           { return network_; }

}  // namespace syntext
