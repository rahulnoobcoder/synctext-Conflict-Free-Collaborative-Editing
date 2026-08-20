#include "ui.h"

#include <algorithm>
#include <sstream>
#include <string>

namespace syntext {

// Color pair indices:
//   1       = status bar (black on white)
//   2       = local cursor (white on blue)
//   3..3+N  = remote peer cursors (rotating palette)
static constexpr int kStatusBarPair = 1;
static constexpr int kLocalCursorPair = 2;
static constexpr int kPeerPairBase = 3;  // peers get 3, 4, 5, ... 3+kMaxPeerColors-1

Ui::Ui() = default;
Ui::~Ui() { teardown(); }

bool Ui::init() {
  win_ = initscr();
  if (!win_) return false;
  raw();               // raw key input
  noecho();            // don't echo typed chars
  keypad(win_, TRUE);  // enable arrow keys / function keys
  nodelay(win_, TRUE); // non-blocking getch
  curs_set(0);         // hide hardware cursor; we draw our own

  if (has_colors()) {
    start_color();
    // use_default_colors() allows -1 as a colour index for terminal defaults.
    // PDCurses' Win32 console backend (wincon) may not implement this; guard it
    // so the build succeeds even with older PDCurses versions.
#ifndef _WIN32
    use_default_colors();
#endif
    // Status bar
    init_pair(kStatusBarPair,    COLOR_BLACK,   COLOR_WHITE);
    // Local cursor
    init_pair(kLocalCursorPair,  COLOR_WHITE,   COLOR_BLUE);
    // Remote peer cursor palette
    short peer_bg_colors[kMaxPeerColors] = {
        COLOR_RED, COLOR_GREEN, COLOR_MAGENTA,
        COLOR_CYAN, COLOR_YELLOW, COLOR_WHITE};
    for (int i = 0; i < kMaxPeerColors; ++i) {
      init_pair(kPeerPairBase + i, COLOR_BLACK, peer_bg_colors[i]);
    }
  }
  return true;
}

void Ui::teardown() {
  if (win_) {
    endwin();
    win_ = nullptr;
  }
}

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------
int Ui::lineCount(const std::string& text) {
  if (text.empty()) return 1;
  int n = 1;
  for (char c : text) if (c == '\n') ++n;
  return n;
}

void Ui::posToLineCol(const std::string& text, int pos,
                      int& out_line, int& out_col) {
  out_line = 0; out_col = 0;
  int p = 0;
  for (char c : text) {
    if (p == pos) return;
    if (c == '\n') { ++out_line; out_col = 0; }
    else            { ++out_col; }
    ++p;
  }
}

int Ui::lineLength(const std::string& text, int line_idx) {
  int line = 0, len = 0;
  for (char c : text) {
    if (line == line_idx) {
      if (c == '\n') break;
      ++len;
    }
    if (c == '\n') ++line;
  }
  return len;
}

int Ui::lineStartPos(const std::string& text, int line_idx) {
  if (line_idx == 0) return 0;
  int line = 0, pos = 0;
  for (char c : text) {
    if (c == '\n') {
      ++line;
      if (line == line_idx) return pos + 1;
    }
    ++pos;
  }
  return static_cast<int>(text.size());
}

// ---------------------------------------------------------------------------
// peerColorPair — assign a stable color to each remote peer.
// ---------------------------------------------------------------------------
int Ui::peerColorPair(const std::string& peer_id) {
  auto it = peer_colors_.find(peer_id);
  if (it != peer_colors_.end()) return it->second;
  int pair = kPeerPairBase + ((next_color_idx_++ - 1) % kMaxPeerColors);
  peer_colors_[peer_id] = pair;
  return pair;
}

// ---------------------------------------------------------------------------
// updatePeerCursor
// ---------------------------------------------------------------------------
void Ui::updatePeerCursor(const std::string& peer_id, CharID after_id) {
  peer_cursors_[peer_id] = after_id;
}

// ---------------------------------------------------------------------------
// localCursorAfterID — returns the stable anchor CharID
// ---------------------------------------------------------------------------
CharID Ui::localCursorAfterID(const CrdtDocument& /*doc*/) const {
  return cursor_anchor_;
}

// ---------------------------------------------------------------------------
// render
// ---------------------------------------------------------------------------
void Ui::render(const CrdtDocument& doc, const std::string& user_id) {
  if (!win_) return;

  int cursor_pos_ = 0;
  if (cursor_anchor_.is_head()) {
    cursor_pos_ = 0;
  } else {
    int p = doc.positionOfCharID(cursor_anchor_);
    if (p >= 0) {
      cursor_pos_ = p + 1;
    } else {
      cursor_pos_ = std::min(cursor_fallback_pos_, doc.visibleCharCount());
    }
  }

  int rows, cols;
  getmaxyx(win_, rows, cols);
  int text_rows = rows - 1;  // one row reserved for status bar

  std::string text = doc.toString();
  int total_lines  = lineCount(text);

  // Clamp scroll_top_ so we don't scroll past the end.
  scroll_top_ = std::max(0, std::min(scroll_top_, total_lines - 1));

  // Compute local cursor screen (y,x) to scroll viewport to reveal it.
  int cur_line = 0, cur_col = 0;
  posToLineCol(text, cursor_pos_, cur_line, cur_col);
  // Scroll down if cursor below viewport.
  if (cur_line >= scroll_top_ + text_rows) scroll_top_ = cur_line - text_rows + 1;
  // Scroll up if cursor above viewport.
  if (cur_line < scroll_top_)             scroll_top_ = cur_line;

  // Build peer-cursor position map: CharID → {peer_id, screen (y,x)}.
  // We'll mark cells as cursor cells when drawing.
  // visible-char-index → peer_id
  std::unordered_map<int, std::string> cursor_at_pos;
  {
    // For each peer cursor, find its visible position.
    // after_id = head() means cursor is before position 0.
    // otherwise, cursor is after the visible position of after_id.
    for (const auto& kv : peer_cursors_) {
      const std::string& pid = kv.first;
      const CharID& aid = kv.second;
      int pos = 0;
      if (!aid.is_head()) {
        // positionOfCharID returns -1 if the character isn't visible yet
        // (e.g. the InsertChar op hasn't arrived/merged yet).  In that case
        // we simply skip drawing this peer's cursor this frame rather than
        // snapping it to position 0.
        int p = doc.positionOfCharID(aid);
        if (p < 0) continue;   // anchor not resolvable yet — skip this frame
        pos = p + 1;           // cursor sits *after* the anchor character
      }
      cursor_at_pos[pos] = pid;
    }
  }

  // Draw text content.
  werase(win_);
  int screen_row = 0;
  int screen_col = 0;
  int doc_line   = 0;

  // Skip lines before scroll_top_.
  // Count visible chars up to the start of scroll_top_.
  int start_pos = lineStartPos(text, scroll_top_);
  doc_line = scroll_top_;
  screen_row = 0;

  for (int pos = start_pos;
       pos <= static_cast<int>(text.size()) && screen_row < text_rows;
       ++pos) {

    // Draw local cursor highlight at this position.
    bool is_local_cursor = (pos == cursor_pos_);
    // Draw remote cursor highlight.
    auto peer_it = cursor_at_pos.find(pos);
    bool is_peer_cursor = (peer_it != cursor_at_pos.end());

    char draw_ch = (pos < static_cast<int>(text.size())) ? text[pos] : ' ';

    if (is_local_cursor) {
      wattron(win_, COLOR_PAIR(kLocalCursorPair));
      mvwaddch(win_, screen_row, screen_col, (draw_ch == '\n' || pos == (int)text.size()) ? ' ' : draw_ch);
      wattroff(win_, COLOR_PAIR(kLocalCursorPair));
    } else if (is_peer_cursor) {
      int cpair = peerColorPair(peer_it->second);
      wattron(win_, COLOR_PAIR(cpair));
      mvwaddch(win_, screen_row, screen_col, (draw_ch == '\n' || pos == (int)text.size()) ? ' ' : draw_ch);
      wattroff(win_, COLOR_PAIR(cpair));
    } else {
      if (pos < static_cast<int>(text.size()) && draw_ch != '\n') {
        mvwaddch(win_, screen_row, screen_col, draw_ch);
      }
    }

    if (pos == static_cast<int>(text.size())) break;

    if (text[pos] == '\n') {
      ++doc_line;
      ++screen_row;
      screen_col = 0;
    } else {
      ++screen_col;
      if (screen_col >= cols) {
        screen_col = 0;
        ++screen_row;
      }
    }
  }

  // Status bar on last row.
  {
    wattron(win_, COLOR_PAIR(kStatusBarPair));
    std::string status = " SyncText  " + user_id;
    // Append peer info.
    if (!peer_cursors_.empty()) {
      status += " | peers:";
      for (const auto& kv : peer_cursors_) {
        status += " " + kv.first;
      }
    }
    // Pad to full width.
    while (static_cast<int>(status.size()) < cols) status += ' ';
    mvwaddstr(win_, rows - 1, 0, status.substr(0, static_cast<size_t>(cols)).c_str());
    wattroff(win_, COLOR_PAIR(kStatusBarPair));
  }

  wnoutrefresh(win_);
  doupdate();
}

// ---------------------------------------------------------------------------
// handleKey
// ---------------------------------------------------------------------------
KeyResult Ui::handleKey(int key,
                         CrdtDocument& doc,
                         const std::string& user_id,
                         LamportClock& clock,
                         uint64_t& seq) {
  KeyResult result;

  if (key == ERR) return result;  // no input

  int cursor_pos_ = 0;
  if (cursor_anchor_.is_head()) {
    cursor_pos_ = 0;
  } else {
    int p = doc.positionOfCharID(cursor_anchor_);
    if (p >= 0) {
      cursor_pos_ = p + 1;
    } else {
      cursor_pos_ = std::min(cursor_fallback_pos_, doc.visibleCharCount());
    }
  }

  int n = doc.visibleCharCount();

  // --- Ctrl-Q / ESC = quit ---
  if (key == ('q' & 0x1f) || key == 27) {
    result.quit = true;
    return result;
  }

  // --- Arrow keys —  cursor movement only ---
  if (key == KEY_LEFT) {
    if (cursor_pos_ > 0) --cursor_pos_;
  } else if (key == KEY_RIGHT) {
    if (cursor_pos_ < n) ++cursor_pos_;
  } else if (key == KEY_UP) {
    std::string text = doc.toString();
    int cur_line, cur_col;
    posToLineCol(text, cursor_pos_, cur_line, cur_col);
    if (cur_line > 0) {
      int above_start = lineStartPos(text, cur_line - 1);
      int above_len   = lineLength(text, cur_line - 1);
      cursor_pos_ = above_start + std::min(cur_col, above_len);
    } else {
      cursor_pos_ = 0;
    }
  } else if (key == KEY_DOWN) {
    std::string text = doc.toString();
    int cur_line, cur_col;
    posToLineCol(text, cursor_pos_, cur_line, cur_col);
    int total = lineCount(text);
    if (cur_line + 1 < total) {
      int below_start = lineStartPos(text, cur_line + 1);
      int below_len   = lineLength(text, cur_line + 1);
      cursor_pos_ = below_start + std::min(cur_col, below_len);
    } else {
      cursor_pos_ = n;
    }
  } else if (key == KEY_HOME) {
    std::string text = doc.toString();
    int cur_line, cur_col;
    posToLineCol(text, cursor_pos_, cur_line, cur_col);
    cursor_pos_ = lineStartPos(text, cur_line);
  } else if (key == KEY_END) {
    std::string text = doc.toString();
    int cur_line, cur_col;
    posToLineCol(text, cursor_pos_, cur_line, cur_col);
    cursor_pos_ = lineStartPos(text, cur_line) + lineLength(text, cur_line);
  }
  // --- Page Up / Page Down ---
  else if (key == KEY_PPAGE) {
    int rows, cols_unused;
    getmaxyx(win_, rows, cols_unused);
    (void)cols_unused;
    scroll_top_ = std::max(0, scroll_top_ - (rows - 1));
  } else if (key == KEY_NPAGE) {
    int rows, cols_unused;
    getmaxyx(win_, rows, cols_unused);
    (void)cols_unused;
    scroll_top_ += rows - 1;
  }
  // --- Backspace / Delete ---
  else if (key == KEY_BACKSPACE || key == 127 || key == '\b') {
    if (cursor_pos_ > 0) {
      CharID target = doc.visibleCharID(cursor_pos_ - 1);
      Operation op;
      op.op_id     = user_id + ":" + std::to_string(seq++);
      op.user_id   = user_id;
      op.timestamp = clock.tick();
      op.type      = OpType::DeleteChar;
      op.target_id = target;
      op.after_id  = CharID::head();
      op.new_char_id = CharID::head();
      op.ch        = 0;
      doc.applyOperation(op);
      result.ops.push_back(op);
      --cursor_pos_;
    }
  } else if (key == KEY_DC) {  // Forward delete
    if (cursor_pos_ < n) {
      CharID target = doc.visibleCharID(cursor_pos_);
      Operation op;
      op.op_id     = user_id + ":" + std::to_string(seq++);
      op.user_id   = user_id;
      op.timestamp = clock.tick();
      op.type      = OpType::DeleteChar;
      op.target_id = target;
      op.after_id  = CharID::head();
      op.new_char_id = CharID::head();
      op.ch        = 0;
      doc.applyOperation(op);
      result.ops.push_back(op);
      // cursor_pos_ stays (next char slides into position)
    }
  }
  // --- Printable characters + Enter ---
  else if (key == '\n' || key == '\r' || key == KEY_ENTER ||
           (key >= 32 && key < 127)) {
    char ch = (key == '\r' || key == KEY_ENTER) ? '\n' : static_cast<char>(key);
    CharID after_id = (cursor_pos_ > 0) ? doc.visibleCharID(cursor_pos_ - 1)
                                        : CharID::head();
    CharID new_id = {user_id, seq};
    Operation op;
    op.op_id      = user_id + ":" + std::to_string(seq++);
    op.user_id    = user_id;
    op.timestamp  = clock.tick();
    op.type       = OpType::InsertChar;
    op.after_id   = after_id;
    op.new_char_id = new_id;
    op.target_id  = CharID::head();
    op.ch         = ch;
    doc.applyOperation(op);
    result.ops.push_back(op);
    ++cursor_pos_;
  }

  // Update the stable anchor based on the new cursor_pos_
  cursor_anchor_ = (cursor_pos_ > 0) ? doc.visibleCharID(cursor_pos_ - 1) : CharID::head();
  cursor_fallback_pos_ = cursor_pos_;

  // Emit cursor broadcast for all key events (ops or movement).
  {
    CursorMessage cm;
    cm.user_id  = user_id;
    cm.after_id = cursor_anchor_;
    result.cursor_json = serializeCursor(cm);
  }

  return result;
}

}  // namespace syntext
