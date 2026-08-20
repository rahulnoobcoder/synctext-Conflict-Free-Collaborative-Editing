#include "network.h"

// All platform-specific socket includes are pulled in via socket_compat.h,
// which is included transitively through network.h.

#include <cstring>
#include <sstream>
#include <thread>

// Bring platform helpers into this translation unit without full qualification.
using namespace syntext::platform;

namespace syntext {

namespace {
const uint16_t kDiscoveryPort = 9999;
const int kPollTimeoutMs = 50;
const uint64_t kDiscoveryIntervalMs = 1000;
const int kQueueRetryMax = 5000;

template <typename Queue, typename Item>
void pushWithRetry(Queue& queue, const Item& item) {
  for (int i = 0; i < kQueueRetryMax; ++i) {
    if (queue.push(item)) {
      return;
    }
    std::this_thread::yield();
  }
  logInfo("queue", "Dropping inbound op after retries");
}
}

Network::Network(const std::string& user_id,
                 uint16_t port,
                 SpscQueue<std::string, 4096>& outbound,
                 SpscQueue<Operation, 16384>& inbound,
                 SpscQueue<std::string, 256>& new_peer_syncs,
                 SpscQueue<CursorMessage, 512>& inbound_cursors)
    : user_id_(user_id),
      port_(port),
      outbound_(outbound),
      inbound_(inbound),
      new_peer_syncs_(new_peer_syncs),
      inbound_cursors_(inbound_cursors) {}

bool Network::setupSockets() {
  // Initialise Winsock on Windows (no-op on POSIX).
  if (!platformInit()) {
    return false;
  }

  tcp_listen_fd_ = platformSocket(AF_INET, SOCK_STREAM, 0);
  if (tcp_listen_fd_ == INVALID_PLATFORM_SOCKET) {
    return false;
  }
  int opt = 1;
  platformSetSockOpt(tcp_listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port_);
  if (platformBind(tcp_listen_fd_,
                   reinterpret_cast<sockaddr*>(&addr),
                   static_cast<int>(sizeof(addr))) != 0) {
    return false;
  }
  if (platformListen(tcp_listen_fd_, 16) != 0) {
    return false;
  }

  udp_fd_ = platformSocket(AF_INET, SOCK_DGRAM, 0);
  if (udp_fd_ == INVALID_PLATFORM_SOCKET) {
    return false;
  }
  platformSetSockOpt(udp_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  platformSetSockOpt(udp_fd_, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));

  sockaddr_in uaddr{};
  uaddr.sin_family = AF_INET;
  uaddr.sin_addr.s_addr = INADDR_ANY;
  uaddr.sin_port = htons(kDiscoveryPort);
  if (platformBind(udp_fd_,
                   reinterpret_cast<sockaddr*>(&uaddr),
                   static_cast<int>(sizeof(uaddr))) != 0) {
    return false;
  }

  platformSetNonBlocking(tcp_listen_fd_);
  platformSetNonBlocking(udp_fd_);
  return true;
}

void Network::closeSockets() {
  if (tcp_listen_fd_ != INVALID_PLATFORM_SOCKET) {
    platformClose(tcp_listen_fd_);
    tcp_listen_fd_ = INVALID_PLATFORM_SOCKET;
  }
  if (udp_fd_ != INVALID_PLATFORM_SOCKET) {
    platformClose(udp_fd_);
    udp_fd_ = INVALID_PLATFORM_SOCKET;
  }
  for (auto& peer : peers_) {
    if (peer.socket_fd != INVALID_PLATFORM_SOCKET) {
      platformClose(peer.socket_fd);
    }
  }
  peers_.clear();

  // Release Winsock on Windows (no-op on POSIX).
  platformCleanup();
}

void Network::broadcastDiscovery() {
  std::string payload = "{\"type\":\"hello\",\"user_id\":\"" + jsonEscape(user_id_) +
                        "\",\"port\":" + std::to_string(port_) + "}";
  sockaddr_in baddr{};
  baddr.sin_family = AF_INET;
  baddr.sin_addr.s_addr = INADDR_BROADCAST;
  baddr.sin_port = htons(kDiscoveryPort);
  platformSendTo(udp_fd_, payload.data(), static_cast<int>(payload.size()),
                 PLATFORM_MSG_NOSIGNAL,
                 reinterpret_cast<sockaddr*>(&baddr),
                 static_cast<int>(sizeof(baddr)));
}

void Network::loadManualPeers() {
  std::string content;
  if (!readFile("peers.conf", content)) {
    return;
  }
  std::istringstream ss(content);
  std::string line;
  while (std::getline(ss, line)) {
    if (line.empty()) {
      continue;
    }
    size_t colon = line.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    std::string ip = line.substr(0, colon);
    uint16_t port = static_cast<uint16_t>(std::stoi(line.substr(colon + 1)));

    bool exists = false;
    for (const auto& peer : peers_) {
      if (peer.user_id == ip + ":" + std::to_string(port)) {
        exists = true;
        break;
      }
    }
    if (exists) {
      continue;
    }

    PlatformSocket fd = platformSocket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_PLATFORM_SOCKET) {
      continue;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (platformInetPton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
      platformClose(fd);
      continue;
    }
    if (platformConnect(fd,
                        reinterpret_cast<sockaddr*>(&addr),
                        static_cast<int>(sizeof(addr))) != 0) {
      platformClose(fd);
      continue;
    }

    Peer peer;
    peer.socket_fd = fd;
    peer.user_id = ip + ":" + std::to_string(port);
    peer.handshake_done = true;
    peer.needs_sync = true;  // request full op-log snapshot from Editor
    new_peer_count_.fetch_add(1, std::memory_order_release);
    peers_.push_back(peer);
    std::string hello = "{\"type\":\"hello\",\"user_id\":\"" + jsonEscape(user_id_) + "\"}";
    sendFrame(fd, hello);
  }
}

bool Network::parseHello(const std::string& json, std::string& out_user_id) {
  if (json.find("\"type\":\"hello\"") == std::string::npos) {
    return false;
  }
  size_t key = json.find("\"user_id\"");
  if (key == std::string::npos) {
    return false;
  }
  size_t colon = json.find(':', key);
  if (colon == std::string::npos) {
    return false;
  }
  size_t start = json.find('"', colon);
  if (start == std::string::npos) {
    return false;
  }
  size_t end = json.find('"', start + 1);
  if (end == std::string::npos) {
    return false;
  }
  out_user_id = jsonUnescape(json.substr(start + 1, end - start - 1));
  return true;
}

void Network::handleUdpDiscovery() {
  sockaddr_in src{};
  platform_socklen_t len = static_cast<platform_socklen_t>(sizeof(src));
  char buffer[512];
  platform_ssize_t received = platformRecvFrom(
      udp_fd_, buffer, static_cast<int>(sizeof(buffer) - 1),
      0,
      reinterpret_cast<sockaddr*>(&src), &len);
  if (received <= 0) {
    return;
  }
  buffer[received] = '\0';
  std::string msg(buffer);
  std::string peer_id;
  if (!parseHello(msg, peer_id)) {
    return;
  }
  if (peer_id == user_id_) {
    return;
  }

  size_t port_pos = msg.find("\"port\"");
  if (port_pos == std::string::npos) {
    return;
  }
  size_t colon = msg.find(':', port_pos);
  if (colon == std::string::npos) {
    return;
  }
  uint16_t port = static_cast<uint16_t>(std::stoi(msg.substr(colon + 1)));

  std::string ip = platformInetNtoa(src.sin_addr);
  for (const auto& peer : peers_) {
    if (peer.socket_fd != INVALID_PLATFORM_SOCKET && peer.user_id == peer_id) {
      return;
    }
  }

  PlatformSocket fd = platformSocket(AF_INET, SOCK_STREAM, 0);
  if (fd == INVALID_PLATFORM_SOCKET) {
    return;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  platformInetPton(AF_INET, ip.c_str(), &addr.sin_addr);
  if (platformConnect(fd,
                      reinterpret_cast<sockaddr*>(&addr),
                      static_cast<int>(sizeof(addr))) != 0) {
    platformClose(fd);
    return;
  }

  Peer peer;
  peer.socket_fd = fd;
  peer.user_id = peer_id;
  peer.handshake_done = true;
  peer.needs_sync = true;  // request full op-log snapshot from Editor
  new_peer_count_.fetch_add(1, std::memory_order_release);
  peers_.push_back(peer);

  std::string hello = "{\"type\":\"hello\",\"user_id\":\"" + jsonEscape(user_id_) + "\"}";
  sendFrame(fd, hello);
}

void Network::acceptTcp() {
  sockaddr_in addr{};
  platform_socklen_t len = static_cast<platform_socklen_t>(sizeof(addr));
  PlatformSocket fd = platformAccept(tcp_listen_fd_,
                                     reinterpret_cast<sockaddr*>(&addr), &len);
  if (fd == INVALID_PLATFORM_SOCKET) {
    return;
  }
  platformSetNonBlocking(fd);
  Peer peer;
  peer.socket_fd = fd;
  peer.handshake_done = false;
  peers_.push_back(peer);
}

void Network::sendFrame(PlatformSocket fd, const std::string& payload) {
  uint32_t len = htonl(static_cast<uint32_t>(payload.size()));
  platformSend(fd, &len, static_cast<int>(sizeof(len)),
               PLATFORM_MSG_NOSIGNAL);
  platformSend(fd, payload.data(), static_cast<int>(payload.size()),
               PLATFORM_MSG_NOSIGNAL);
}

void Network::readFromPeer(size_t index) {
  Peer& peer = peers_[index];
  char buffer[4096];
  platform_ssize_t received = platformRecv(peer.socket_fd, buffer,
                                            static_cast<int>(sizeof(buffer)), 0);
  if (received <= 0) {
    removePeer(index);
    return;
  }
  peer.recv_buffer.append(buffer, static_cast<size_t>(received));

  while (true) {
    if (peer.expected_len == 0) {
      if (peer.recv_buffer.size() < sizeof(uint32_t)) {
        return;
      }
      uint32_t len = 0;
      std::memcpy(&len, peer.recv_buffer.data(), sizeof(uint32_t));
      peer.expected_len = ntohl(len);
      peer.recv_buffer.erase(0, sizeof(uint32_t));
    }
    if (peer.recv_buffer.size() < peer.expected_len) {
      return;
    }
    std::string payload = peer.recv_buffer.substr(0, peer.expected_len);
    peer.recv_buffer.erase(0, peer.expected_len);
    peer.expected_len = 0;

    // Cursor broadcast: route directly to inbound_cursors_.
    if (payload.find("\"type\":\"cursor\"") != std::string::npos) {
      CursorMessage cm;
      if (parseCursor(payload, cm)) {
        inbound_cursors_.push(cm);
      }
      continue;
    }

    if (payload.find("\"type\":\"hello\"") != std::string::npos) {
      std::string peer_id;
      if (parseHello(payload, peer_id)) {
        peer.user_id = peer_id;
        peer.handshake_done = true;
        peer.needs_sync = true;
        new_peer_count_.fetch_add(1, std::memory_order_release);
      }
      continue;
    }

    Message msg;
    if (parseMessage(payload, msg)) {
      for (const auto& op : msg.ops) {
        pushWithRetry(inbound_, op);
      }
    }
  }
}

void Network::removePeer(size_t index) {
  if (index >= peers_.size()) {
    return;
  }
  if (peers_[index].socket_fd != INVALID_PLATFORM_SOCKET) {
    platformClose(peers_[index].socket_fd);
  }
  peers_.erase(peers_.begin() + static_cast<long>(index));
}

void Network::sendOutbound() {
  std::string payload;
  while (outbound_.pop(payload)) {
    for (size_t i = 0; i < peers_.size(); ++i) {
      if (peers_[i].socket_fd == INVALID_PLATFORM_SOCKET) {
        continue;
      }
      sendFrame(peers_[i].socket_fd, payload);
    }
  }
}

// Called once per network loop iteration. If any peer has needs_sync=true and
// the Editor has pushed a snapshot into new_peer_syncs_, deliver it to every
// waiting peer.
void Network::sendPendingSyncs() {
  // Count how many peers are waiting for a sync.
  size_t waiting = 0;
  for (const auto& p : peers_) {
    if (p.needs_sync && p.socket_fd != INVALID_PLATFORM_SOCKET) ++waiting;
  }
  if (waiting == 0) return;

  // Drain one snapshot message from the queue (Editor pushes one per request).
  std::string snapshot;
  if (!new_peer_syncs_.pop(snapshot)) return;

  for (auto& p : peers_) {
    if (p.needs_sync && p.socket_fd != INVALID_PLATFORM_SOCKET) {
      sendFrame(p.socket_fd, snapshot);
      p.needs_sync = false;
    }
  }
}

void Network::run(std::atomic<bool>& running) {
  if (!setupSockets()) {
    logInfo("network", "Failed to set up sockets");
    return;
  }

  loadManualPeers();

  uint64_t last_broadcast = 0;

  while (running.load(std::memory_order_acquire)) {
    uint64_t now = nowMillis();
    if (now - last_broadcast >= kDiscoveryIntervalMs) {
      broadcastDiscovery();
      last_broadcast = now;
    }

    // Build poll list using the portable platform_pollfd type.
    std::vector<platform_pollfd> fds;
    fds.reserve(2 + peers_.size());
    fds.push_back({tcp_listen_fd_, POLLIN, 0});
    fds.push_back({udp_fd_, POLLIN, 0});
    for (const auto& peer : peers_) {
      fds.push_back({peer.socket_fd, POLLIN, 0});
    }

    int ret = platformPoll(fds.data(),
                           static_cast<unsigned>(fds.size()),
                           kPollTimeoutMs);
    if (ret > 0) {
      size_t idx = 0;
      if (fds[idx++].revents & POLLIN) {
        acceptTcp();
      }
      if (fds[idx++].revents & POLLIN) {
        handleUdpDiscovery();
      }
      for (size_t i = 0; i < peers_.size(); ++i, ++idx) {
        if (fds[idx].revents & POLLIN) {
          readFromPeer(i);
          if (i >= peers_.size()) {
            break;
          }
        }
      }
    }

    sendPendingSyncs();
    sendOutbound();
  }

  closeSockets();
}

}  // namespace syntext
