#ifndef REPLICATION_WORKER_H
#define REPLICATION_WORKER_H

// Asynchronous replication worker
// Processes replication jobs in background thread

#define MAX_REPL_FILENAME 256
#define MAX_REPL_SS_NAME 64

// Replication operation types
typedef enum {
    REPL_OP_CREATE,      // Replicate file creation
    REPL_OP_DELETE,      // Replicate file deletion
    REPL_OP_UPDATE,      // Replicate file content update
    REPL_OP_METADATA,    // Replicate metadata only
    REPL_OP_SYNC_ALL     // Full sync (all files)
} ReplicationOp;

// Replication job
typedef struct ReplicationJob {
    ReplicationOp operation;
    char filename[MAX_REPL_FILENAME];
    char primary_ss[MAX_REPL_SS_NAME];
    char replica_ss[MAX_REPL_SS_NAME];
    struct ReplicationJob *next;
} ReplicationJob;

// Initialize replication worker
void replication_worker_init(void);

// Start worker thread
int replication_worker_start(void);

// Stop worker thread
void replication_worker_stop(void);

// Queue a replication job (async, non-blocking)
// Returns 0 on success, -1 if queue is full
int replication_worker_queue(ReplicationOp operation,
                              const char *filename,
                              const char *primary_ss,
                              const char *replica_ss);

// Get statistics (for monitoring)
typedef struct {
    int pending_jobs;
    int completed_jobs;
    int failed_jobs;
} ReplicationStats;

void replication_worker_get_stats(ReplicationStats *stats);

#endif
