// crdt_test.cpp — unit tests for the character-level CRDT merge engine.
// Build with: make test
// Run with:   ./tests/crdt_test

#include <algorithm>
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include "../src/crdt.h"
#include "../src/utils.h"

using namespace syntext;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void CHECK(bool cond, const char* msg) {
  if (!cond) {
    std::cerr << "[FAIL] " << msg << "\n";
    std::exit(1);
  }
}

// Build an InsertChar op: insert character `ch` after `after_id`,
// assigning `new_char_id` to the new character.
static Operation makeInsert(const std::string& user_id,
                             LamportClock& clock,
                             uint64_t& seq,
                             CharID after_id,
                             char ch) {
  Operation op;
  op.op_id      = user_id + ":" + std::to_string(seq);
  op.user_id    = user_id;
  op.timestamp  = clock.tick();
  op.type       = OpType::InsertChar;
  op.after_id   = after_id;
  op.new_char_id = {user_id, seq++};
  op.target_id  = CharID::head();
  op.ch         = ch;
  return op;
}

// Build a DeleteChar op targeting `target_id`.
static Operation makeDelete(const std::string& user_id,
                             LamportClock& clock,
                             uint64_t& seq,
                             CharID target_id) {
  Operation op;
  op.op_id     = user_id + ":" + std::to_string(seq++);
  op.user_id   = user_id;
  op.timestamp = clock.tick();
  op.type      = OpType::DeleteChar;
  op.target_id = target_id;
  op.after_id  = CharID::head();
  op.new_char_id = CharID::head();
  op.ch        = 0;
  return op;
}

// Apply all op batches in sorted Lamport order and return the final text.
static std::string mergeAll(const std::string& base,
                             const std::vector<std::vector<Operation>>& all_ops) {
  std::vector<Operation> ops;
  for (const auto& batch : all_ops)
    for (const auto& op : batch) ops.push_back(op);

  std::sort(ops.begin(), ops.end(), [](const Operation& a, const Operation& b) {
    if (a.timestamp != b.timestamp) return a.timestamp < b.timestamp;
    if (a.user_id   != b.user_id)   return a.user_id   < b.user_id;
    return a.op_id < b.op_id;
  });

  CrdtDocument doc;
  doc.loadFromString(base);
  doc.applyOperations(ops);
  return doc.toString();
}

// ---------------------------------------------------------------------------
// Test 1: Concurrent inserts at different anchors (the original bug repro,
//         now at char level).
//
//   base = "AC"
//   User A inserts 'B' after 'A' (after pos 0).
//   User B inserts 'D' after 'C' (after pos 1) concurrently.
//   Expected: "ABCD"
// ---------------------------------------------------------------------------
static void test1_concurrent_inserts_different_anchors() {
  const std::string base = "AC";

  CrdtDocument baseDoc;
  baseDoc.loadFromString(base);

  LamportClock cA, cB;
  uint64_t sA = 1, sB = 1;

  CharID id_A = baseDoc.visibleCharID(0);  // 'A'
  CharID id_C = baseDoc.visibleCharID(1);  // 'C'

  auto opsA = std::vector<Operation>{makeInsert("uA", cA, sA, id_A, 'B')};
  auto opsB = std::vector<Operation>{makeInsert("uB", cB, sB, id_C, 'D')};

  std::string merged = mergeAll(base, {opsA, opsB});
  std::cout << "[test1] merged = \"" << merged << "\"\n";
  CHECK(merged == "ABCD",
        "test1: concurrent inserts at different anchors gave wrong result");
  std::cout << "[test1] PASS\n";
}

// ---------------------------------------------------------------------------
// Test 2: Concurrent inserts at the SAME anchor — deterministic ordering
//         by (timestamp, user_id). "uA" < "uB" so when timestamps tie,
//         uA's char comes first (lower user_id wins tie in stampWins ↔ comes
//         later in position because stampWins(existing,incoming) skips forward;
//         so the char with HIGHER stamp is placed first — which is uB when
//         both have same ts. Check both replicas agree.)
// ---------------------------------------------------------------------------
static void test2_concurrent_inserts_same_anchor() {
  const std::string base = "XZ";

  CrdtDocument baseDoc;
  baseDoc.loadFromString(base);

  LamportClock cA, cB;
  uint64_t sA = 1, sB = 1;

  // Force same timestamp.
  cA.update(10); cB.update(10);

  CharID id_X = baseDoc.visibleCharID(0);  // 'X'

  auto opsA = std::vector<Operation>{makeInsert("uA", cA, sA, id_X, 'Y')};
  auto opsB = std::vector<Operation>{makeInsert("uB", cB, sB, id_X, 'W')};

  std::string r1 = mergeAll(base, {opsA, opsB});
  std::string r2 = mergeAll(base, {opsB, opsA});

  std::cout << "[test2] r1=\"" << r1 << "\" r2=\"" << r2 << "\"\n";
  CHECK(r1 == r2, "test2: concurrent inserts at same anchor — replicas differ");
  CHECK(r1.find('Y') != std::string::npos, "test2: 'Y' missing");
  CHECK(r1.find('W') != std::string::npos, "test2: 'W' missing");
  CHECK(r1.find('X') != std::string::npos, "test2: 'X' missing");
  CHECK(r1.find('Z') != std::string::npos, "test2: 'Z' missing");
  std::cout << "[test2] PASS\n";
}

// ---------------------------------------------------------------------------
// Test 3: Concurrent delete + insert at the same char.
//         A (ts=20) deletes 'B'. B (ts=5) tries to delete 'B' too.
//         Result: 'B' is gone (first delete wins, second is idempotent).
// ---------------------------------------------------------------------------
static void test3_concurrent_delete_same_char() {
  const std::string base = "AB";

  CrdtDocument baseDoc;
  baseDoc.loadFromString(base);

  LamportClock cA, cB;
  uint64_t sA = 1, sB = 1;

  cA.update(20);
  cB.update(4);  // next tick → ts=5

  CharID id_B = baseDoc.visibleCharID(1);

  auto opsA = std::vector<Operation>{makeDelete("uA", cA, sA, id_B)};
  auto opsB = std::vector<Operation>{makeDelete("uB", cB, sB, id_B)};

  std::string merged = mergeAll(base, {opsA, opsB});
  std::cout << "[test3] merged = \"" << merged << "\"\n";
  CHECK(merged == "A", "test3: both deletes should leave only 'A'");
  std::cout << "[test3] PASS\n";
}

// ---------------------------------------------------------------------------
// Test 4: Three-user interleaved inserts — all 6 apply orders converge.
//         base = "", A inserts "Hi", B inserts "!" at end, C inserts " " after 'i'.
// ---------------------------------------------------------------------------
static void test4_three_user_convergence() {
  const std::string base = "Hi";  // H=pos0, i=pos1

  CrdtDocument baseDoc;
  baseDoc.loadFromString(base);

  LamportClock cA, cB, cC;
  uint64_t sA = 1, sB = 1, sC = 1;
  cA.update(10); cB.update(20); cC.update(30);

  CharID id_i = baseDoc.visibleCharID(1);  // 'i'
  CharID id_H = baseDoc.visibleCharID(0);  // 'H'

  // A: insert '!' after 'i'
  auto opsA = std::vector<Operation>{makeInsert("uA", cA, sA, id_i, '!')};
  // B: insert ' ' after 'H'
  auto opsB = std::vector<Operation>{makeInsert("uB", cB, sB, id_H, ' ')};
  // C: insert ',' after 'i'
  auto opsC = std::vector<Operation>{makeInsert("uC", cC, sC, id_i, ',')};

  std::string r1 = mergeAll(base, {opsA, opsB, opsC});
  std::string r2 = mergeAll(base, {opsB, opsC, opsA});
  std::string r3 = mergeAll(base, {opsC, opsA, opsB});
  std::string r4 = mergeAll(base, {opsA, opsC, opsB});
  std::string r5 = mergeAll(base, {opsB, opsA, opsC});
  std::string r6 = mergeAll(base, {opsC, opsB, opsA});

  std::cout << "[test4] r1=\"" << r1 << "\"\n";
  CHECK(r1 == r2 && r2 == r3 && r3 == r4 && r4 == r5 && r5 == r6,
        "test4: three-user convergence — replicas differ");
  CHECK(r1.find('H') != std::string::npos, "test4: 'H' missing");
  CHECK(r1.find('i') != std::string::npos, "test4: 'i' missing");
  CHECK(r1.find('!') != std::string::npos, "test4: '!' missing");
  CHECK(r1.find(' ') != std::string::npos, "test4: ' ' missing");
  CHECK(r1.find(',') != std::string::npos, "test4: ',' missing");
  std::cout << "[test4] PASS\n";
}

// ---------------------------------------------------------------------------
// Test 5: Idempotent replay — double-apply must not change the document.
// ---------------------------------------------------------------------------
static void test5_idempotent_replay() {
  const std::string base = "hello";

  CrdtDocument baseDoc;
  baseDoc.loadFromString(base);

  LamportClock clock;
  uint64_t seq = 1;

  CharID id_o = baseDoc.visibleCharID(4);
  auto ops = std::vector<Operation>{makeInsert("u1", clock, seq, id_o, '!')};

  CrdtDocument doc;
  doc.loadFromString(base);
  doc.applyOperations(ops);
  std::string first = doc.toString();

  doc.applyOperations(ops);  // replay same ops
  std::string second = doc.toString();

  std::cout << "[test5] doc = \"" << first << "\"\n";
  CHECK(first == second, "test5: double-apply changed document (not idempotent)");
  CHECK(first == "hello!", "test5: wrong content");
  std::cout << "[test5] PASS\n";
}

// ---------------------------------------------------------------------------
// Test 6: Two users type a word character by character concurrently, fully
//         interleaved.  The merged result must contain ALL typed characters
//         from both users with no loss and no duplication.
//
//   User A types "CAT" starting from an empty doc.
//   User B types "DOG" starting from an empty doc.
//   Both start at the head sentinel. Since their timestamps are unique, RGA
//   gives a deterministic, stable ordering. Final doc contains exactly 6 chars:
//   {C,A,T,D,O,G} in some deterministic order; both replicas agree.
// ---------------------------------------------------------------------------
static void test6_concurrent_typing() {
  const std::string base = "";

  LamportClock cA, cB;
  uint64_t sA = 1, sB = 1;

  std::vector<Operation> opsA, opsB;

  // A types C, A, T sequentially (each after the previous one they inserted).
  {
    CrdtDocument docA;
    docA.loadFromString(base);

    Operation opC = makeInsert("uA", cA, sA, CharID::head(), 'C');
    docA.applyOperation(opC);
    opsA.push_back(opC);

    Operation opA = makeInsert("uA", cA, sA, opC.new_char_id, 'A');
    docA.applyOperation(opA);
    opsA.push_back(opA);

    Operation opT = makeInsert("uA", cA, sA, opA.new_char_id, 'T');
    opsA.push_back(opT);
  }

  // B types D, O, G sequentially.
  {
    CrdtDocument docB;
    docB.loadFromString(base);

    Operation opD = makeInsert("uB", cB, sB, CharID::head(), 'D');
    docB.applyOperation(opD);
    opsB.push_back(opD);

    Operation opO = makeInsert("uB", cB, sB, opD.new_char_id, 'O');
    docB.applyOperation(opO);
    opsB.push_back(opO);

    Operation opG = makeInsert("uB", cB, sB, opO.new_char_id, 'G');
    opsB.push_back(opG);
  }

  std::string r1 = mergeAll(base, {opsA, opsB});
  std::string r2 = mergeAll(base, {opsB, opsA});

  std::cout << "[test6] r1=\"" << r1 << "\" r2=\"" << r2 << "\"\n";
  CHECK(r1 == r2,    "test6: concurrent typing — replicas differ");
  CHECK(r1.size() == 6, "test6: wrong total character count");
  CHECK(r1.find('C') != std::string::npos, "test6: 'C' missing");
  CHECK(r1.find('A') != std::string::npos, "test6: 'A' missing");
  CHECK(r1.find('T') != std::string::npos, "test6: 'T' missing");
  CHECK(r1.find('D') != std::string::npos, "test6: 'D' missing");
  CHECK(r1.find('O') != std::string::npos, "test6: 'O' missing");
  CHECK(r1.find('G') != std::string::npos, "test6: 'G' missing");
  // No duplication.
  for (char c : {'C','A','T','D','O','G'}) {
    long cnt = std::count(r1.begin(), r1.end(), c);
    CHECK(cnt == 1, "test6: character duplicated in merged result");
  }
  std::cout << "[test6] PASS\n";
}

// ---------------------------------------------------------------------------
// Test 7: Local cursor anchor tracking.
//         Reproduces the headless UI logic exactly: stable CharID anchors.
//         User A and User B type concurrently at far apart lines.
//         A's ops arrive while B is typing. B's anchor should prevent B's
//         next keystroke from landing in the wrong place.
// ---------------------------------------------------------------------------
static void test7_local_cursor_anchor_tracking() {
  std::string base = "line1\nline2\nline3\nline4\nline5\nline6\nline7\nline8\nline9\nline10\n";
  CrdtDocument docA, docB;
  docA.loadFromString(base);
  docB.loadFromString(base);

  LamportClock cA, cB;
  uint64_t sA = 1, sB = 1;

  // A's cursor is after "line1"
  int posA = 5;
  CharID anchorA = docA.visibleCharID(posA - 1);
  int fallbackA = posA;

  // B's cursor is after "line10"
  int posB = base.size() - 1;
  CharID anchorB = docB.visibleCharID(posB - 1);
  int fallbackB = posB;

  // A types 'A'
  posA = anchorA.is_head() ? 0 : docA.positionOfCharID(anchorA) + 1;
  CharID new_afterA = (posA > 0) ? docA.visibleCharID(posA - 1) : CharID::head();
  Operation opA1 = makeInsert("uA", cA, sA, new_afterA, 'A');
  docA.applyOperation(opA1);
  posA++;
  anchorA = (posA > 0) ? docA.visibleCharID(posA - 1) : CharID::head();
  fallbackA = posA;

  // B's document receives A's operation (simulating network arrival)
  docB.applyOperation(opA1);

  // Now B types 'B'. Without anchor tracking, posB would still be at the end, 
  // but A's insertion shifted the document length! 
  // With anchor tracking, B calculates posB from its stable anchor.
  posB = anchorB.is_head() ? 0 : docB.positionOfCharID(anchorB);
  if (posB >= 0) posB += 1; else posB = std::min(fallbackB, docB.visibleCharCount());
  
  CharID new_afterB = (posB > 0) ? docB.visibleCharID(posB - 1) : CharID::head();
  Operation opB1 = makeInsert("uB", cB, sB, new_afterB, 'B');
  docB.applyOperation(opB1);
  posB++;
  anchorB = (posB > 0) ? docB.visibleCharID(posB - 1) : CharID::head();
  fallbackB = posB;

  std::string result = docB.toString();
  std::cout << "[test7] result = \n" << result << "\n";
  // Verify 'A' is exactly after "line1"
  CHECK(result.find("line1A\nline2") != std::string::npos, "test7: A inserted in wrong place");
  // Verify 'B' is exactly after "line10"
  CHECK(result.find("line10B\n") != std::string::npos, "test7: B inserted in wrong place");

  std::cout << "[test7] PASS\n";
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
  std::cout << "=== crdt_test (char level) ===\n";
  test1_concurrent_inserts_different_anchors();
  test2_concurrent_inserts_same_anchor();
  test3_concurrent_delete_same_char();
  test4_three_user_convergence();
  test5_idempotent_replay();
  test6_concurrent_typing();
  test7_local_cursor_anchor_tracking();
  std::cout << "=== All 7 tests PASSED ===\n";
  return 0;
}
