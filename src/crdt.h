#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "utils.h"

namespace syntext {

// ---------------------------------------------------------------------------
// CharID — stable, globally-unique identity for a single character.
// Same structure as the previous LineID; renamed and applied at char level.
// ---------------------------------------------------------------------------
struct CharID {
  std::string user_id;
  uint64_t    seq = 0;

  bool operator==(const CharID& o) const {
    return seq == o.seq && user_id == o.user_id;
  }
  bool operator!=(const CharID& o) const { return !(*this == o); }

  // Sentinel: "before the first character in the document."
  static CharID head() { return {"__head__", 0}; }
  bool is_head() const { return user_id == "__head__"; }

  std::string toString() const { return user_id + ":" + std::to_string(seq); }

  static CharID fromString(const std::string& s) {
    auto pos = s.rfind(':');
    if (pos == std::string::npos) return head();
    CharID id;
    id.user_id = s.substr(0, pos);
    id.seq     = static_cast<uint64_t>(std::stoull(s.substr(pos + 1)));
    return id;
  }
};

// ---------------------------------------------------------------------------
// Operations — character granularity.
// A "replace" decomposes naturally into DeleteChar + InsertChar(s).
// ---------------------------------------------------------------------------
enum class OpType {
  InsertChar,
  DeleteChar
};

struct Operation {
  std::string op_id;
  std::string user_id;
  uint64_t    timestamp  = 0;
  OpType      type       = OpType::InsertChar;

  // InsertChar: insert new_char_id after after_id.
  // DeleteChar: tombstone target_id.
  CharID after_id;      // InsertChar
  CharID target_id;     // DeleteChar
  CharID new_char_id;   // InsertChar — stable ID of the new character

  char   ch = 0;        // the character being inserted (InsertChar)
};

struct Message {
  std::string            sender_id;
  std::vector<Operation> ops;
};

// Cursor-position broadcast — sent immediately (not batched with ops).
struct CursorMessage {
  std::string user_id;
  CharID      after_id;   // cursor sits after this character (head = before all)
};

// ---------------------------------------------------------------------------
// CrdtDocument
// ---------------------------------------------------------------------------
class CrdtDocument {
 public:
  CrdtDocument();

  void loadFromString(const std::string& content);
  std::string toString() const;

  // Stable CharID of the visible character at 0-based position pos.
  // Returns CharID::head() for pos < 0 or if the document is empty.
  CharID visibleCharID(int pos) const;

  // Number of non-deleted, non-head characters.
  int visibleCharCount() const;

  // Character value at visible position pos (-1 if out of range).
  int charAt(int pos) const;

  // Returns the current 0-based VISIBLE position of the character with this stable ID,
  // or -1 if the ID doesn't exist or is deleted. Returns 0 if id is head().
  int positionOfCharID(const CharID& id) const;

  bool applyOperation(const Operation& op);
  bool applyOperations(const std::vector<Operation>& ops);

 private:
  struct CharEntry {
    CharID      id;
    char        ch    = 0;
    Stamp       stamp;
    bool        deleted = false;
  };

  // O(1) lookup from serialized CharID → index in chars_.
  // Kept in sync on every insert/delete.
  std::unordered_map<std::string, size_t> id_index_;

  int findByID(const CharID& id) const;
  void rebuildIndex();

  std::vector<CharEntry>           chars_;
  std::unordered_set<std::string>  applied_ops_;
};

// ---------------------------------------------------------------------------
// Serialization helpers
// ---------------------------------------------------------------------------
std::string opTypeToString(OpType type);
bool opTypeFromString(const std::string& value, OpType& out);

std::string serializeMessage(const Message& msg);
bool parseMessage(const std::string& json, Message& out);

std::string serializeCursor(const CursorMessage& cursor);
bool parseCursor(const std::string& json, CursorMessage& out);

}  // namespace syntext
