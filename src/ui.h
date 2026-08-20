#pragma once

#include <string>
#include <unordered_map>
#include <vector>

// PDCurses (Windows) provides the same API as ncurses under <curses.h>.
// On Linux/macOS we keep the canonical <ncurses.h> include.
#ifdef _WIN32
#  include <curses.h>
#else
#  include <ncurses.h>
#endif

#include "crdt.h"
#include "utils.h"

namespace syntext {

// Maximum number of distinct remote-user colors (wraps around after this many).
static constexpr int kMaxPeerColors = 6;

// Result of a single key-handling cycle.
struct KeyResult {
  std::vector<Operation> ops;       // CRDT ops to push (may be empty)
  std::string            cursor_json; // serialized CursorMessage (empty if no move)
  bool                   quit = false;
};

class Ui {
 public:
  Ui();
  ~Ui();

  bool init();
  void teardown();

  // Process one ncurses key event against the given document.
  // Generates CRDT ops and/or a cursor broadcast.
  KeyResult handleKey(int key,
                      CrdtDocument& doc,
                      const std::string& user_id,
                      LamportClock& clock,
                      uint64_t& seq);

  // Render the full document into the ncurses window.
  // Called after any op is applied (local or remote).
  void render(const CrdtDocument& doc, const std::string& user_id);

  // Update a remote peer's cursor position.
  void updatePeerCursor(const std::string& peer_id, CharID after_id);

  // Returns the CharID after which the local cursor currently sits.
  CharID localCursorAfterID(const CrdtDocument& doc) const;

 private:
  WINDOW* win_        = nullptr;
  CharID  cursor_anchor_ = CharID::head();    // anchor for exactly where the local cursor sits
  int     cursor_fallback_pos_ = 0;           // numeric fallback position if anchor is deleted
  int     scroll_top_ = 0;    // first visible line index

  // peer_id → CharID of their cursor (after_id)
  std::unordered_map<std::string, CharID> peer_cursors_;
  // peer_id → color-pair index (1..kMaxPeerColors)
  std::unordered_map<std::string, int>    peer_colors_;
  int next_color_idx_ = 1;

  int peerColorPair(const std::string& peer_id);

  // Compute (line, col) from a linear visible-char position,
  // given the document content string `text`.
  static void posToLineCol(const std::string& text, int pos,
                            int& out_line, int& out_col);

  // Return char count of line `line_idx` (0-based) in `text`.
  static int lineLength(const std::string& text, int line_idx);

  // Count number of lines (newline-separated) in `text`.
  static int lineCount(const std::string& text);

  // Offset (in visible-char positions) of the start of `line_idx`.
  static int lineStartPos(const std::string& text, int line_idx);
};

}  // namespace syntext
