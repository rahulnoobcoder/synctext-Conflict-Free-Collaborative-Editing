#include "crdt.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace syntext {

CrdtDocument::CrdtDocument() = default;

// ---------------------------------------------------------------------------
// loadFromString — one CharEntry per character.
// Head sentinel at index 0 (always deleted=true so toString skips it).
// Base characters get IDs {".base", 1..n}.
// ---------------------------------------------------------------------------
void CrdtDocument::loadFromString(const std::string& content) {
  chars_.clear();
  id_index_.clear();
  applied_ops_.clear();

  // Head sentinel.
  CharEntry head;
  head.id      = CharID::head();
  head.ch      = 0;
  head.stamp   = {0, ""};
  head.deleted = true;
  chars_.push_back(head);
  id_index_[head.id.toString()] = 0;

  uint64_t seq = 1;
  for (char c : content) {
    CharEntry entry;
    entry.id      = {".base", seq++};
    entry.ch      = c;
    entry.stamp   = {0, ""};
    entry.deleted = false;
    id_index_[entry.id.toString()] = chars_.size();
    chars_.push_back(entry);
  }
}

// ---------------------------------------------------------------------------
// toString — collect non-deleted, non-head chars.
// ---------------------------------------------------------------------------
std::string CrdtDocument::toString() const {
  std::string out;
  out.reserve(chars_.size());
  for (const auto& e : chars_) {
    if (!e.id.is_head() && !e.deleted) {
      out.push_back(e.ch);
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// findByID — O(1) via id_index_.
// ---------------------------------------------------------------------------
int CrdtDocument::findByID(const CharID& id) const {
  auto it = id_index_.find(id.toString());
  if (it == id_index_.end()) return -1;
  return static_cast<int>(it->second);
}

void CrdtDocument::rebuildIndex() {
  id_index_.clear();
  for (size_t i = 0; i < chars_.size(); ++i) {
    id_index_[chars_[i].id.toString()] = i;
  }
}

// ---------------------------------------------------------------------------
// visibleCharID — stable ID of visible character at 0-based position pos.
// ---------------------------------------------------------------------------
CharID CrdtDocument::visibleCharID(int pos) const {
  if (pos < 0) return CharID::head();
  int visible = 0;
  for (const auto& e : chars_) {
    if (e.id.is_head() || e.deleted) continue;
    if (visible == pos) return e.id;
    ++visible;
  }
  return CharID::head();  // out of range
}

int CrdtDocument::visibleCharCount() const {
  int count = 0;
  for (const auto& e : chars_) {
    if (!e.id.is_head() && !e.deleted) ++count;
  }
  return count;
}

int CrdtDocument::charAt(int pos) const {
  if (pos < 0) return -1;
  int visible = 0;
  for (const auto& e : chars_) {
    if (e.id.is_head() || e.deleted) continue;
    if (visible == pos) return static_cast<unsigned char>(e.ch);
    ++visible;
  }
  return -1;
}

// ---------------------------------------------------------------------------
// positionOfCharID — returns 0-based visible index string position of a stable ID.
// ---------------------------------------------------------------------------
int CrdtDocument::positionOfCharID(const CharID& id) const {
  if (id.is_head()) return 0;
  
  int internal_idx = findByID(id);
  if (internal_idx < 0) return -1;
  const auto& e = chars_[static_cast<size_t>(internal_idx)];
  if (e.deleted) return -1;

  int visible_pos = 0;
  // Count non-deleted, non-head chars *before* this index
  for (int i = 1; i < internal_idx; ++i) { // i=1 to skip head
    if (!chars_[i].deleted) {
      ++visible_pos;
    }
  }
  // The position of the character is the number of valid chars before it.
  // Wait, if it's the first actual char, visible_pos is 0? Yes, but head is pos 0.
  // Wait, visibleCharID(0) returns the FIRST non-deleted char. 
  // Let's check visibleCharID:
  // if (pos < 0) return head; 
  // if (visible == pos) return e.id;
  // So visibleCharID(0) is the first char.
  // If we count non-deleted chars before `internal_idx`, that is exactly its 0-based index.
  // BUT wait, localCursorAfterID expects head() for cursor at start (cursor_pos_ == 0).
  // cursor_pos corresponds to positions *between* characters.
  // If cursor is at 0, after_id = head.
  // If cursor is at N, after_id = id of visible char N-1.
  // Our function finds the index of a char.
  // If `id` is the first visible char, visible chars before it = 0. So positionOfCharID returns 0.
  // But wait! If positionOfCharID returns 0, and we use cursor_anchor_, then
  // cursor_pos = 0 is head.
  // Wait, let's verify how UI uses the returned int.
  // Let `id` = visibleCharID(cursor_pos_ - 1). 
  // We want to recover `cursor_pos_`.
  // If cursor_pos_ was 1, id = visibleCharID(0). 
  // positionOfCharID(id) returns 0.
  // We then set cursor_pos_ = 0 + 1 = 1.
  // Ah! positionOfCharID should return the index of the char.
  // So `cursor_pos_ = (cursor_anchor_.is_head()) ? 0 : doc.positionOfCharID(cursor_anchor_) + 1`.
  // Yes! The position of the cursor is the position of the character it follows, plus 1.
  
  return visible_pos;
}

// ---------------------------------------------------------------------------
// applyOperation — identical RGA anchor+scan+stampWins logic from the former
// line-level implementation, now applied per character.
// ---------------------------------------------------------------------------
bool CrdtDocument::applyOperation(const Operation& op) {
  if (applied_ops_.count(op.op_id)) return false;
  applied_ops_.insert(op.op_id);

  if (op.type == OpType::InsertChar) {
    // Find anchor (after_id). Default to head (index 0) if not found.
    int anchor = findByID(op.after_id);
    if (anchor < 0) anchor = 0;

    // RGA scan: skip past entries at the same anchor that have higher stamp,
    // so concurrent inserts after the same anchor converge identically everywhere.
    Stamp incoming{op.timestamp, op.user_id};
    size_t insert_pos = static_cast<size_t>(anchor) + 1;
    while (insert_pos < chars_.size()) {
      if (!stampWins(chars_[insert_pos].stamp, incoming)) break;
      ++insert_pos;
    }

    CharEntry entry;
    entry.id      = op.new_char_id;
    entry.ch      = op.ch;
    entry.stamp   = incoming;
    entry.deleted = false;

    chars_.insert(chars_.begin() + static_cast<std::ptrdiff_t>(insert_pos), entry);

    // Rebuild index — insert shifted all indices after insert_pos.
    // Efficient: only update entries from insert_pos onward.
    for (size_t i = insert_pos; i < chars_.size(); ++i) {
      id_index_[chars_[i].id.toString()] = i;
    }
    return true;
  }

  if (op.type == OpType::DeleteChar) {
    int idx = findByID(op.target_id);
    if (idx < 0) return false;
    CharEntry& e = chars_[static_cast<size_t>(idx)];
    if (stampWins({op.timestamp, op.user_id}, e.stamp)) {
      e.deleted = true;
      e.stamp   = {op.timestamp, op.user_id};
      return true;
    }
    return false;
  }

  return false;
}

bool CrdtDocument::applyOperations(const std::vector<Operation>& ops) {
  bool changed = false;
  for (const auto& op : ops) {
    if (applyOperation(op)) changed = true;
  }
  return changed;
}

// ---------------------------------------------------------------------------
// OpType helpers
// ---------------------------------------------------------------------------
std::string opTypeToString(OpType type) {
  return (type == OpType::InsertChar) ? "insert_char" : "delete_char";
}

bool opTypeFromString(const std::string& value, OpType& out) {
  if (value == "insert_char") { out = OpType::InsertChar; return true; }
  if (value == "delete_char") { out = OpType::DeleteChar; return true; }
  return false;
}

// ---------------------------------------------------------------------------
// JSON helpers (unchanged from previous version)
// ---------------------------------------------------------------------------
static bool extractString(const std::string& json, const std::string& key,
                           std::string& out) {
  std::string token = "\"" + key + "\"";
  size_t pos = json.find(token);
  if (pos == std::string::npos) return false;
  pos = json.find(':', pos);
  if (pos == std::string::npos) return false;
  pos = json.find('"', pos);
  if (pos == std::string::npos) return false;
  size_t end = pos + 1;
  bool escape = false;
  for (; end < json.size(); ++end) {
    char c = json[end];
    if (escape) { escape = false; continue; }
    if (c == '\\') { escape = true; continue; }
    if (c == '"') break;
  }
  if (end >= json.size()) return false;
  out = jsonUnescape(json.substr(pos + 1, end - pos - 1));
  return true;
}

static bool extractUint64(const std::string& json, const std::string& key,
                           uint64_t& out) {
  std::string token = "\"" + key + "\"";
  size_t pos = json.find(token);
  if (pos == std::string::npos) return false;
  pos = json.find(':', pos);
  if (pos == std::string::npos) return false;
  ++pos;
  while (pos < json.size() &&
         std::isspace(static_cast<unsigned char>(json[pos])))
    ++pos;
  size_t end = pos;
  while (end < json.size() &&
         std::isdigit(static_cast<unsigned char>(json[end])))
    ++end;
  if (end == pos) return false;
  out = static_cast<uint64_t>(std::stoull(json.substr(pos, end - pos)));
  return true;
}

static std::vector<std::string> splitObjects(const std::string& array_text) {
  std::vector<std::string> objects;
  int    depth     = 0;
  bool   in_string = false;
  size_t start     = std::string::npos;
  for (size_t i = 0; i < array_text.size(); ++i) {
    char c = array_text[i];
    if (c == '"' && (i == 0 || array_text[i - 1] != '\\'))
      in_string = !in_string;
    if (in_string) continue;
    if (c == '{') {
      if (depth == 0) start = i;
      ++depth;
    } else if (c == '}') {
      --depth;
      if (depth == 0 && start != std::string::npos) {
        objects.push_back(array_text.substr(start, i - start + 1));
        start = std::string::npos;
      }
    }
  }
  return objects;
}

// ---------------------------------------------------------------------------
// serializeMessage / parseMessage
// ---------------------------------------------------------------------------
std::string serializeMessage(const Message& msg) {
  std::ostringstream ss;
  ss << "{\"sender_id\":\"" << jsonEscape(msg.sender_id) << "\",\"ops\":[";
  for (size_t i = 0; i < msg.ops.size(); ++i) {
    const auto& op = msg.ops[i];
    ss << "{\"op_id\":\""      << jsonEscape(op.op_id)   << "\"";
    ss << ",\"user_id\":\""    << jsonEscape(op.user_id) << "\"";
    ss << ",\"timestamp\":"    << op.timestamp;
    ss << ",\"type\":\""       << opTypeToString(op.type) << "\"";
    ss << ",\"after_id\":\""   << jsonEscape(op.after_id.toString())    << "\"";
    ss << ",\"target_id\":\""  << jsonEscape(op.target_id.toString())   << "\"";
    ss << ",\"new_char_id\":\"" << jsonEscape(op.new_char_id.toString()) << "\"";
    // Store char as single escaped JSON string.
    std::string ch_str(1, op.ch);
    ss << ",\"ch\":\""         << jsonEscape(ch_str) << "\"";
    ss << "}";
    if (i + 1 < msg.ops.size()) ss << ",";
  }
  ss << "]}";
  return ss.str();
}

bool parseMessage(const std::string& json, Message& out) {
  out = Message{};
  if (!extractString(json, "sender_id", out.sender_id)) return false;

  size_t ops_pos = json.find("\"ops\"");
  if (ops_pos == std::string::npos) return true;
  size_t array_start = json.find('[', ops_pos);
  size_t array_end   = json.rfind(']');
  if (array_start == std::string::npos || array_end == std::string::npos)
    return false;

  std::string array_text =
      json.substr(array_start, array_end - array_start + 1);
  for (const auto& obj : splitObjects(array_text)) {
    Operation op;
    if (!extractString(obj, "op_id", op.op_id)) continue;
    extractString(obj, "user_id", op.user_id);
    extractUint64(obj, "timestamp", op.timestamp);
    std::string type_str;
    if (extractString(obj, "type", type_str)) opTypeFromString(type_str, op.type);

    std::string id_str;
    if (extractString(obj, "after_id",    id_str)) op.after_id    = CharID::fromString(id_str);
    if (extractString(obj, "target_id",   id_str)) op.target_id   = CharID::fromString(id_str);
    if (extractString(obj, "new_char_id", id_str)) op.new_char_id = CharID::fromString(id_str);

    std::string ch_str;
    if (extractString(obj, "ch", ch_str) && !ch_str.empty()) op.ch = ch_str[0];

    out.ops.push_back(op);
  }
  return true;
}

// ---------------------------------------------------------------------------
// Cursor message serialize/parse
// ---------------------------------------------------------------------------
std::string serializeCursor(const CursorMessage& cursor) {
  std::ostringstream ss;
  ss << "{\"type\":\"cursor\"";
  ss << ",\"user_id\":\""  << jsonEscape(cursor.user_id)          << "\"";
  ss << ",\"after_id\":\"" << jsonEscape(cursor.after_id.toString()) << "\"";
  ss << "}";
  return ss.str();
}

bool parseCursor(const std::string& json, CursorMessage& out) {
  if (json.find("\"type\":\"cursor\"") == std::string::npos) return false;
  if (!extractString(json, "user_id", out.user_id)) return false;
  std::string id_str;
  if (!extractString(json, "after_id", id_str)) return false;
  out.after_id = CharID::fromString(id_str);
  return true;
}

}  // namespace syntext
