# SyncText Design

## System architecture diagram (textual)

```
+---------------------------+       TCP Updates       +---------------------------+
|   Peer A (editor user_1)  | <---------------------> |   Peer B (editor user_2)  |
|                           |                         |                           |
|  Main Thread              |                         |  Main Thread              |
|  - Poll file              |                         |  - Poll file              |
|  - Create ops             |                         |  - Create ops             |
|  - Batch + enqueue ops    |                         |  - Batch + enqueue ops    |
|                           |                         |                           |
|  Network Thread           | --- UDP broadcast --->  |  Network Thread           |
|  - Discovery              |                         |  - Discovery              |
|  - TCP send/recv          |                         |  - TCP send/recv          |
|                           |                         |                           |
|  Merge Thread             |                         |  Merge Thread             |
|  - Apply CRDT             |                         |  - Apply CRDT             |
|  - Write doc file         |                         |  - Write doc file         |
+---------------------------+                         +---------------------------+
```

## Thread model

- **Main thread**
  - Polls file changes.
  - Generates operations.
  - Batches and enqueues outgoing messages.

- **Network thread**
  - UDP broadcast for discovery.
  - TCP listener and receiver.
  - Pushes remote operations to the inbound queue.

- **Merge thread**
  - Merges local and remote operations.
  - Applies CRDT rules.
  - Writes updates to disk and refreshes the terminal UI.

## Data structures

- `Operation`
  - `op_id`: unique id (`user_id:seq`)
  - `timestamp`: Lamport clock
  - `type`: insert/delete/replace
  - `line`, `col_start`, `col_end`
  - `old_content`, `new_content`

- `CrdtDocument`
  - Line-based LWW registers.
  - Tombstones to preserve line indices under deletes.
  - Idempotent operation application.

- `SpscQueue`
  - Lock-free single-producer/single-consumer ring buffer.
  - Used for thread communication without mutexes.

## Lock-free reasoning

- Each queue has a single producer and a single consumer.
- No shared mutable state is accessed by multiple threads without a queue boundary.
- Network peer list is owned by the network thread only.

## Conflict resolution

- Conflicts: same line and overlapping columns.
- LWW rule applied to the line register:
  - Higher timestamp wins.
  - If equal, smaller `user_id` wins.

## Trade-offs

- Line-based CRDT simplifies merging and keeps operations compact.
- Per-character CRDT would be more precise but significantly more complex.
- TCP ensures reliable delivery; UDP is only used for discovery.
