# SyncText

SyncText is a **CRDT-based collaborative text editor** that runs fully peer-to-peer across machines on the same LAN. Each instance provides a terminal UI (ncurses/PDCurses) and exchanges character-level operations with peers over TCP, using UDP broadcast for automatic peer discovery.

## How it Works

- Each peer runs `./editor <user_id>` and opens a full-screen terminal editor backed by `<user_id>_doc.txt`.
- Keystrokes generate CRDT operations (RGA-style, character-level).
- Operations are broadcast to connected peers in real time.
- A conflict-free merge algorithm applies operations using deterministic ordering (Lamport timestamp + user ID tie-break), ensuring all peers converge to the same document without a central server.
- Peer discovery is automatic via UDP broadcast; manual `peers.conf` is supported as a fallback.

---

## Architecture

| Component | Description |
|---|---|
| `src/crdt.h/.cpp` | RGA-style CRDT with unique CharIDs, tombstoned deletion, idempotent dedup |
| `src/network.h/.cpp` | TCP/UDP networking with portable socket abstraction |
| `src/platform/socket_compat.h` | Winsock2 / POSIX socket abstraction layer |
| `src/ui.h/.cpp` | ncurses (Linux/macOS) / PDCurses (Windows) terminal UI |
| `src/editor.h/.cpp` | Main orchestrator: UI loop, merge loop, op batching |
| `src/thread_manager.h/.cpp` | Launches network + merge threads |
| `src/utils.h/.cpp` | Lamport clock, SPSC queue, JSON helpers |
| `tests/crdt_test.cpp` | 7 unit tests for the CRDT merge engine |

---

## Building

### Linux / macOS

#### Prerequisites
```bash
sudo apt-get install g++ make libncurses-dev   # Debian/Ubuntu
brew install ncurses                            # macOS (Homebrew)
```

#### Build with Make (classic)
```bash
make          # builds ./editor
make test     # builds and runs tests/crdt_test
make clean
```

#### Build with CMake (alternative)
```bash
cmake -B build
cmake --build build
ctest --test-dir build -V      # run CRDT unit tests
```

---

### Windows

#### Prerequisites

| Tool | Notes |
|---|---|
| CMake ≥ 3.16 | [cmake.org](https://cmake.org/download/) |
| MSVC 2019/2022 **or** MinGW-w64 (g++) | Visual Studio or [MSYS2](https://www.msys2.org/) |
| PDCurses | See options below |

#### Option A — vcpkg (recommended for MSVC)

[vcpkg](https://vcpkg.io) is the easiest way to get PDCurses on Windows.

```powershell
# 1. Install vcpkg (one-time setup)
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat

# 2. Install PDCurses
C:\vcpkg\vcpkg install pdcurses:x64-windows

# 3. Configure and build SyncText
cmake -B build -G "Visual Studio 17 2022" `
      -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Release

# 4. Run
.\editor.exe alice
```

#### Option B — Manual PDCurses install (MinGW / MSYS2)

```bash
# In MSYS2 MINGW64 shell:
pacman -S mingw-w64-x86_64-pdcurses   # installs to /mingw64/

cmake -B build -G "MinGW Makefiles" \
      -DPDCURSES_ROOT=/mingw64
cmake --build build

./editor.exe alice
```

#### Option C — Pre-built PDCurses binary (MSVC)

1. Download a pre-built PDCurses release from [https://pdcurses.org](https://pdcurses.org) and extract to, for example, `C:\libs\pdcurses`.
2. The expected layout is:
   ```
   C:\libs\pdcurses\
     include\curses.h
     lib\pdcurses.lib
   ```
3. Configure CMake with `-DPDCURSES_ROOT=C:\libs\pdcurses`:
   ```powershell
   cmake -B build -G "Visual Studio 17 2022" -DPDCURSES_ROOT=C:\libs\pdcurses
   cmake --build build --config Release
   .\editor.exe alice
   ```

#### Running CRDT unit tests on Windows
```powershell
# After building with any method above:
ctest --test-dir build -C Release -V
# Or directly:
.\build\Release\crdt_test.exe
```

---

## Running — Two Peers on the Same LAN

### Option 1: Automatic Discovery (UDP Broadcast)

If both machines are on the same WiFi/LAN with no firewall blocking UDP port 9999:

**Machine A:**
```bash
./editor user_A
```

**Machine B:**
```bash
./editor user_B
```

Peers find each other automatically within ~1 second.

### Option 2: Manual Peer Configuration (`peers.conf`)

Useful when UDP broadcast is blocked (common in VMs, cloud instances, or strict firewalls). You need the LAN IP of each machine.

Assume **Machine A** = `192.168.1.10` and **Machine B** = `192.168.1.20`.

**Machine A** — create `peers.conf`:
```
192.168.1.20:9002
```
Start on port 9001:
```bash
./editor user_A 9001
```

**Machine B** — create `peers.conf`:
```
192.168.1.10:9001
```
Start on port 9002:
```bash
./editor user_B 9002
```

Both instances read `peers.conf` at startup and establish a direct TCP connection.

---

## Key Bindings

| Key | Action |
|---|---|
| Any printable key | Insert character at cursor |
| `Enter` | Insert newline |
| `Backspace` / `Del` | Delete character before / at cursor |
| `←` `→` `↑` `↓` | Move cursor |
| `Home` / `End` | Jump to start / end of line |
| `PgUp` / `PgDn` | Scroll one screen |
| `Ctrl-Q` or `Esc` | Quit |

---

## Verifying CRDT Sync

1. Watch both terminal outputs for the peer-connected message in the status bar.
2. Type on Machine A — characters appear on Machine B in real time.
3. Type on **both** machines simultaneously at the same position. Both documents will converge to the identical final state (deterministic Lamport ordering), demonstrating CRDT conflict resolution.

---

## Testing (CRDT Unit Tests)

Seven unit tests cover:

| Test | What it checks |
|---|---|
| `test1` | Concurrent inserts at different anchors |
| `test2` | Concurrent inserts at the same anchor — replicas must agree |
| `test3` | Concurrent deletes of the same char — idempotent |
| `test4` | Three-user convergence (all 6 application orders) |
| `test5` | Double-apply idempotence |
| `test6` | Two users typing concurrently, no character loss or duplication |
| `test7` | Cursor anchor stability under remote insertions |

**Linux/macOS:**
```bash
make test
```

**Windows:**
```powershell
ctest --test-dir build -C Release -V
```

The test binary links only `crdt.cpp` + `utils.cpp` — no networking, no UI, no platform-specific code. It passes unchanged on all platforms.

---

## Project Structure

```
synctext/
├── CMakeLists.txt          # Cross-platform build (Linux/macOS/Windows)
├── Makefile                # Linux/macOS quick build
├── src/
│   ├── platform/
│   │   └── socket_compat.h # Winsock2 / POSIX socket abstraction
│   ├── crdt.h / crdt.cpp   # CRDT algorithm (do not modify)
│   ├── network.h / .cpp    # TCP/UDP networking (portable)
│   ├── ui.h / ui.cpp       # Terminal UI (ncurses / PDCurses)
│   ├── editor.h / .cpp     # Main orchestrator
│   ├── thread_manager.h/.cpp
│   ├── utils.h / utils.cpp
│   └── main.cpp
└── tests/
    └── crdt_test.cpp       # 7 CRDT unit tests (platform-independent)
```
