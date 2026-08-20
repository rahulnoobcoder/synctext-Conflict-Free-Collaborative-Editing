#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace syntext {

struct Stamp {
  uint64_t ts = 0;
  std::string user_id;
};

inline bool stampWins(const Stamp& a, const Stamp& b) {
  if (a.ts != b.ts) {
    return a.ts > b.ts;
  }
  return a.user_id < b.user_id;
}

class LamportClock {
 public:
  LamportClock();

  uint64_t tick();
  uint64_t update(uint64_t remote_ts);
  uint64_t value() const;

 private:
  std::atomic<uint64_t> clock_;
};

uint64_t nowMillis();
uint64_t fnv1aHash(const std::string& data);

bool readFile(const std::string& path, std::string& out);
bool writeFileAtomic(const std::string& path, const std::string& data);

std::vector<std::string> splitLines(const std::string& text);
std::string joinLines(const std::vector<std::string>& lines);

std::string jsonEscape(const std::string& input);
std::string jsonUnescape(const std::string& input);

void logInfo(const std::string& tag, const std::string& message);

// Single-producer single-consumer ring buffer.
template <typename T, size_t Capacity>
class SpscQueue {
 public:
  SpscQueue() : buffer_(Capacity), head_(0), tail_(0) {}

  bool push(const T& item) {
    size_t tail = tail_.load(std::memory_order_relaxed);
    size_t next = (tail + 1) % Capacity;
    if (next == head_.load(std::memory_order_acquire)) {
      return false;
    }
    buffer_[tail] = item;
    tail_.store(next, std::memory_order_release);
    return true;
  }

  bool pop(T& item) {
    size_t head = head_.load(std::memory_order_relaxed);
    if (head == tail_.load(std::memory_order_acquire)) {
      return false;
    }
    item = buffer_[head];
    head_.store((head + 1) % Capacity, std::memory_order_release);
    return true;
  }

  bool empty() const {
    return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
  }

 private:
  std::vector<T> buffer_;
  std::atomic<size_t> head_;
  std::atomic<size_t> tail_;
};

}  // namespace syntext
