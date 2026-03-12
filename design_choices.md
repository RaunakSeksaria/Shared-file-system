# Design Choices & Implementation Notes

This document captures the major design decisions we made when requirements left room for interpretation.

## Concurrency & Threading
- **Thread-per-connection NM**: NM accepts each SS/client socket on a dedicated pthread. This keeps the dispatcher simple; the workload (parsing, registry updates, cache lookups) is CPU-light, so the overhead is acceptable.
- **SS worker pool**: Storage Servers use a fixed-size worker queue with 8 pthreads to serve READ/WRITE/STREAM/ACL requests concurrently. This avoids blocking the accept loop during long WRITE sessions or STREAM delays, matching the TA hint about preferring thread pools.
- **Sentence-level locking**: WRITE locks are tracked in an in-memory runtime state keyed by sentence ID. Locks survive multiple edits inside one WRITE session and prevent DELETE/MOVE while active.

## Networking & Protocol
- **Line-oriented text protocol** over TCP for every hop (Client↔NM, NM↔SS, Client↔SS). Easy to debug and works well with `recv_line`.
- **Direct Client↔SS data path**: NM sends `SS_INFO` with host/port for READ/STREAM/WRITE/UNDO, following the spec while keeping NM out of high-volume file data.
- **STOP packets everywhere**: STREAM, READ, and EXEC all emit explicit STOP markers so clients know when to terminate.

## File & Metadata Handling
- **Lazy loading**: SS loads file contents/metadata only when requested (per HackMD clarification #26). Metadata lives in text files under `storage_ssX/metadata`.
- **Atomic writes**: WRITE/UNDO/CHECKPOINT flows use temp files + rename to guarantee crash-safe updates.
- **Sentence identities**: Each sentence has a stable ID persisted in metadata so locks stay consistent even if earlier edits reindex sentences.

## Access Control
- **ACL source of truth on SS**: NM always fetches ACLs from SS (now cached) to avoid stale permissions.
- **ACL cache in NM**: Recently fetched ACLs are memoized in a 256-entry ring buffer; entries invalidate when ACLs change (ADD/REM, MOVE, DELETE, request approvals).
- **UNDO requires write access**: Aligns with HackMD Q40—only writers (or owners) can revert.
- **Access requests**: NM stores pending requests in memory and persistently in SS metadata so owners can approve/deny later.

## Client & Registry Behavior
- **CLI-driven username**: Client accepts `--username` to simplify automation; we log registration to `registry_clients.txt` so LIST survives NM restarts.
- **Interactive WRITE shell**: Client locks a sentence, shows the current text, and after each edit prints the updated sentence for clarity.

## Logging & Observability
- **JSON-line logs** (`log_info`/`log_error`) in NM and SS include timestamps & event names, satisfying the spec’s logging requirement.
- **Minimal cache logs**: ACL cache operations stay silent to avoid log noise; can be instrumented later if needed.

## Miscellaneous
- **Folder support**: We implemented optional CREATEFOLDER/VIEWFOLDER/MOVE commands with metadata/index updates.
- **Checkpoints**: Optional CHECKPOINT/VIEW/REVERT/LISTCHECKPOINTS commands persist snapshots on SS.
- **Streaming delay**: STREAM sends one word every 0.1 seconds via `nanosleep`, matching the “cinematic” requirement.

These choices aim to balance correctness, debuggability, and the time constraints of the course project. Let us know if you’d like deeper dives on any component.


