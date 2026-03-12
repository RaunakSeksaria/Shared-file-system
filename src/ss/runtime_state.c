#include "runtime_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    FileRuntimeState *head;
    pthread_mutex_t mu;
} RuntimeManager;

static RuntimeManager g_manager = {NULL, PTHREAD_MUTEX_INITIALIZER};

static FileRuntimeState *find_state(const char *filename) {
    if (!filename) return NULL;
    pthread_mutex_lock(&g_manager.mu);
    FileRuntimeState *cur = g_manager.head;
    while (cur) {
        if (strcmp(cur->filename, filename) == 0) {
            pthread_mutex_unlock(&g_manager.mu);
            return cur;
        }
        cur = cur->next;
    }
    pthread_mutex_unlock(&g_manager.mu);
    return NULL;
}

static FileRuntimeState *find_or_create_state(const char *filename) {
    FileRuntimeState *existing = find_state(filename);
    if (existing) return existing;
    if (!filename) return NULL;
    pthread_mutex_lock(&g_manager.mu);
    // ensure not added by race
    FileRuntimeState *cur = g_manager.head;
    while (cur) {
        if (strcmp(cur->filename, filename) == 0) {
            pthread_mutex_unlock(&g_manager.mu);
            return cur;
        }
        cur = cur->next;
    }
    FileRuntimeState *node = calloc(1, sizeof(FileRuntimeState));
    if (!node) {
        pthread_mutex_unlock(&g_manager.mu);
        return NULL;
    }
    strncpy(node->filename, filename, sizeof(node->filename) - 1);
    pthread_mutex_init(&node->lock_mu, NULL);
    node->next_session_id = 1;
    node->next = g_manager.head;
    g_manager.head = node;
    pthread_mutex_unlock(&g_manager.mu);
    return node;
}

void runtime_state_init(void) {
    g_manager.head = NULL;
    pthread_mutex_init(&g_manager.mu, NULL);
}

void runtime_state_shutdown(void) {
    pthread_mutex_lock(&g_manager.mu);
    FileRuntimeState *cur = g_manager.head;
    while (cur) {
        FileRuntimeState *next = cur->next;
        pthread_mutex_destroy(&cur->lock_mu);
        free(cur);
        cur = next;
    }
    g_manager.head = NULL;
    pthread_mutex_unlock(&g_manager.mu);
    pthread_mutex_destroy(&g_manager.mu);
}

static int allocate_session(FileRuntimeState *state) {
    if (state->next_session_id <= 0) {
        state->next_session_id = 1;
    }
    return state->next_session_id++;
}

int sentence_lock_acquire(const char *filename, int sentence_id, const char *username, int *out_session_id) {
    if (!filename || !username || sentence_id <= 0) return -1;
    FileRuntimeState *state = find_or_create_state(filename);
    if (!state) return -1;

    pthread_mutex_lock(&state->lock_mu);
    for (int i = 0; i < state->lock_count; i++) {
        if (state->locks[i].sentence_id == sentence_id) {
            pthread_mutex_unlock(&state->lock_mu);
            return -1; // already locked
        }
    }
    if (state->lock_count >= MAX_RUNTIME_LOCKS) {
        pthread_mutex_unlock(&state->lock_mu);
        return -1;
    }
    int session_id = allocate_session(state);
    SentenceLock *lock = &state->locks[state->lock_count++];
    lock->sentence_id = sentence_id;
    strncpy(lock->locked_by, username, sizeof(lock->locked_by) - 1);
    lock->lock_time = time(NULL);
    lock->session_id = session_id;
    pthread_mutex_unlock(&state->lock_mu);
    if (out_session_id) {
        *out_session_id = session_id;
    }
    return session_id;
}

int sentence_lock_release(const char *filename, int sentence_id, int session_id) {
    if (!filename || sentence_id <= 0 || session_id <= 0) return -1;
    FileRuntimeState *state = find_or_create_state(filename);
    if (!state) return -1;

    int result = -1;
    pthread_mutex_lock(&state->lock_mu);
    for (int i = 0; i < state->lock_count; i++) {
        if (state->locks[i].sentence_id == sentence_id &&
            state->locks[i].session_id == session_id) {
            state->locks[i] = state->locks[state->lock_count - 1];
            state->lock_count--;
            result = 0;
            break;
        }
    }
    pthread_mutex_unlock(&state->lock_mu);
    return result;
}

void sentence_lock_release_all(const char *filename, int session_id) {
    if (!filename || session_id <= 0) return;
    FileRuntimeState *state = find_or_create_state(filename);
    if (!state) return;

    pthread_mutex_lock(&state->lock_mu);
    int write_idx = 0;
    for (int i = 0; i < state->lock_count; i++) {
        if (state->locks[i].session_id == session_id) {
            continue;
        }
        if (write_idx != i) {
            state->locks[write_idx] = state->locks[i];
        }
        write_idx++;
    }
    state->lock_count = write_idx;
    pthread_mutex_unlock(&state->lock_mu);
}

void sentence_lock_cleanup(time_t cutoff_seconds) {
    time_t now = time(NULL);
    pthread_mutex_lock(&g_manager.mu);
    FileRuntimeState *state = g_manager.head;
    while (state) {
        pthread_mutex_lock(&state->lock_mu);
        int write_idx = 0;
        for (int i = 0; i < state->lock_count; i++) {
            time_t age = now - state->locks[i].lock_time;
            if (cutoff_seconds > 0 && age > cutoff_seconds) {
                // drop stale lock
                continue;
            }
            if (write_idx != i) {
                state->locks[write_idx] = state->locks[i];
            }
            write_idx++;
        }
        state->lock_count = write_idx;
        pthread_mutex_unlock(&state->lock_mu);
        state = state->next;
    }
    pthread_mutex_unlock(&g_manager.mu);
}

int runtime_state_has_active_locks(const char *filename) {
    if (!filename) return 0;
    FileRuntimeState *state = find_state(filename);
    if (!state) return 0;
    int has_lock = 0;
    pthread_mutex_lock(&state->lock_mu);
    if (state->lock_count > 0) {
        has_lock = 1;
    }
    pthread_mutex_unlock(&state->lock_mu);
    return has_lock;
}


