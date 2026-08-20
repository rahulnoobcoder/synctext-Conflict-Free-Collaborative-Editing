#include "utils.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

namespace syntext {

LamportClock::LamportClock() : clock_(0) {}

uint64_t LamportClock::tick() {
  return clock_.fetch_add(1, std::memory_order_acq_rel) + 1;
}

uint64_t LamportClock::update(uint64_t remote_ts) {
  uint64_t current = clock_.load(std::memory_order_acquire);
  while (current < remote_ts) {
    if (clock_.compare_exchange_weak(current, remote_ts, std::memory_order_acq_rel)) {
      break;
    }
  }
  return tick();
}

uint64_t LamportClock::value() const {
  return clock_.load(std::memory_order_acquire);
}

uint64_t nowMillis() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

uint64_t fnv1aHash(const std::string& data) {
  const uint64_t kOffset = 1469598103934665603ull;
  const uint64_t kPrime = 1099511628211ull;
  uint64_t hash = kOffset;
  for (unsigned char c : data) {
    hash ^= static_cast<uint64_t>(c);
    hash *= kPrime;
  }
  return hash;
}

bool readFile(const std::string& path, std::string& out) {
  std::ifstream file(path, std::ios::in | std::ios::binary);
  if (!file.is_open()) {
    return false;
  }
  std::ostringstream ss;
  ss << file.rdbuf();
  out = ss.str();
  return true;
}

bool writeFileAtomic(const std::string& path, const std::string& data) {
  std::string tmp_path = path + ".tmp";
  {
    std::ofstream file(tmp_path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
      return false;
    }
    file.write(data.data(), static_cast<std::streamsize>(data.size()));
    file.flush();
    if (!file.good()) {
      return false;
    }
  }
  return std::rename(tmp_path.c_str(), path.c_str()) == 0;
}

std::vector<std::string> splitLines(const std::string& text) {
  std::vector<std::string> lines;
  std::string current;
  for (char c : text) {
    if (c == '\n') {
      lines.push_back(current);
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  lines.push_back(current);
  return lines;
}

std::string joinLines(const std::vector<std::string>& lines) {
  std::ostringstream ss;
  for (size_t i = 0; i < lines.size(); ++i) {
    ss << lines[i];
    if (i + 1 < lines.size()) {
      ss << '\n';
    }
  }
  return ss.str();
}

std::string jsonEscape(const std::string& input) {
  std::string out;
  out.reserve(input.size());
  for (char c : input) {
    switch (c) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(c);
        break;
    }
  }
  return out;
}

std::string jsonUnescape(const std::string& input) {
  std::string out;
  out.reserve(input.size());
  for (size_t i = 0; i < input.size(); ++i) {
    char c = input[i];
    if (c == '\\' && i + 1 < input.size()) {
      char n = input[++i];
      switch (n) {
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        case '\\':
          out.push_back('\\');
          break;
        case '"':
          out.push_back('"');
          break;
        default:
          out.push_back(n);
          break;
      }
    } else {
      out.push_back(c);
    }
  }
  return out;
}

void logInfo(const std::string& tag, const std::string& message) {
  auto ts = nowMillis();
  std::cout << "[" << ts << "][" << tag << "] " << message << std::endl;
}

}  // namespace syntext
