#ifndef RUNTIME_STATE_H
#define RUNTIME_STATE_H

#include <pthread.h>
#include <time.h>

#define MAX_RUNTIME_LOCKS 1024
#define LOCK_TIMEOUT_SECONDS 300  // 5 minutes

typedef struct {
    int sentence_id;
    char locked_by[64];
    time_t lock_time;
    int session_id;
} SentenceLock;

typedef struct FileRuntimeState {
    char filename[256];
    pthread_mutex_t lock_mu;
    SentenceLock locks[MAX_RUNTIME_LOCKS];
    int lock_count;
    int next_session_id;
    struct FileRuntimeState *next;
} FileRuntimeState;

void runtime_state_init(void);
void runtime_state_shutdown(void);

// Acquire lock on a sentence. Returns session_id (>0) on success, -1 on failure.
int sentence_lock_acquire(const char *filename, int sentence_id, const char *username, int *out_session_id);

// Release specific lock.
int sentence_lock_release(const char *filename, int sentence_id, int session_id);

// Release all locks held by a session (e.g., on disconnect).
void sentence_lock_release_all(const char *filename, int session_id);

// Remove locks older than cutoff seconds (used by watchdog).
void sentence_lock_cleanup(time_t cutoff_seconds);

// Returns 1 if any sentence locks exist for the file.
int runtime_state_has_active_locks(const char *filename);

#endif  // RUNTIME_STATE_H


