#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "crdt.h"
#include "platform/socket_compat.h"
#include "utils.h"

namespace syntext {

class Network {
 public:
  Network(const std::string& user_id,
          uint16_t port,
          SpscQueue<std::string, 4096>& outbound,
          SpscQueue<Operation, 16384>& inbound,
          SpscQueue<std::string, 256>& new_peer_syncs,
          SpscQueue<CursorMessage, 512>& inbound_cursors);

  void run(std::atomic<bool>& running);

  // Incremented each time a peer completes a TCP handshake.
  std::atomic<int> new_peer_count_{0};

 private:
  struct Peer {
    platform::PlatformSocket socket_fd    = platform::INVALID_PLATFORM_SOCKET;
    std::string user_id;
    std::string recv_buffer;
    uint32_t    expected_len = 0;
    bool        handshake_done = false;
    bool        needs_sync     = false;
  };

  std::string  user_id_;
  uint16_t     port_;
  SpscQueue<std::string, 4096>& outbound_;
  SpscQueue<Operation, 16384>&  inbound_;
  SpscQueue<std::string, 256>&  new_peer_syncs_;
  SpscQueue<CursorMessage, 512>& inbound_cursors_;

  platform::PlatformSocket tcp_listen_fd_ = platform::INVALID_PLATFORM_SOCKET;
  platform::PlatformSocket udp_fd_        = platform::INVALID_PLATFORM_SOCKET;

  std::vector<Peer> peers_;

  bool setupSockets();
  void closeSockets();
  void handleUdpDiscovery();
  void broadcastDiscovery();
  void loadManualPeers();
  void acceptTcp();
  void readFromPeer(size_t index);
  void removePeer(size_t index);
  void sendOutbound();
  void sendPendingSyncs();
  void sendFrame(platform::PlatformSocket fd, const std::string& payload);
  bool parseHello(const std::string& json, std::string& out_user_id);
};

}  // namespace syntext
