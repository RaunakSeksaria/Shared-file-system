#include "registry.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char username[64];
    int file_count;
} SsCandidate;

static int compare_ss_candidates(const void *a, const void *b) {
    const SsCandidate *ca = (const SsCandidate*)a;
    const SsCandidate *cb = (const SsCandidate*)b;
    if (ca->file_count == cb->file_count) {
        return strcmp(ca->username, cb->username);
    }
    return (ca->file_count < cb->file_count) ? -1 : 1;
}

// Global registry (thread-safe)
static RegistryEntry *g_registry_head = NULL;
static pthread_mutex_t g_registry_mu = PTHREAD_MUTEX_INITIALIZER;

// Persistence for client usernames
#define REGISTRY_PATH_MAX 512
static char g_registry_path[REGISTRY_PATH_MAX];
static int g_registry_persistence_enabled = 0;
static int g_registry_loading = 0;

static void registry_append_client(const char *username) {
    if (!g_registry_persistence_enabled || !username || username[0] == '\0') {
        return;
    }
    FILE *fp = fopen(g_registry_path, "a");
    if (!fp) {
        return;
    }
    fprintf(fp, "%s\n", username);
    fclose(fp);
}

// Add entry to registry
int registry_add(const char *role, const char *username, const char *payload) {
    if (!role || !username) return 0;
    pthread_mutex_lock(&g_registry_mu);
    RegistryEntry *entry = g_registry_head;
    while (entry) {
        if (strcmp(entry->role, role) == 0 &&
            strcmp(entry->username, username) == 0) {
            snprintf(entry->payload, sizeof(entry->payload), "%s", payload ? payload : "");
            pthread_mutex_unlock(&g_registry_mu);
            return 0;
        }
        entry = entry->next;
    }
    RegistryEntry *e = (RegistryEntry*)calloc(1, sizeof(RegistryEntry));
    snprintf(e->role, sizeof(e->role), "%s", role);
    snprintf(e->username, sizeof(e->username), "%s", username);
    snprintf(e->payload, sizeof(e->payload), "%s", payload ? payload : "");
    e->file_count = 0;
    e->next = g_registry_head;
    g_registry_head = e;
    pthread_mutex_unlock(&g_registry_mu);

    if (!g_registry_loading &&
        g_registry_persistence_enabled &&
        strcmp(role, "CLIENT") == 0) {
        registry_append_client(username);
    }
    return 1;
}

void registry_init_persistence(const char *path) {
    if (!path || path[0] == '\0') return;
    snprintf(g_registry_path, sizeof(g_registry_path), "%s", path);
    FILE *fp = fopen(g_registry_path, "a+");
    if (!fp) {
        return;
    }
    g_registry_persistence_enabled = 1;
    rewind(fp);
    g_registry_loading = 1;
    char line[128];
    while (fgets(line, sizeof(line), fp)) {
        char *newline = strpbrk(line, "\r\n");
        if (newline) *newline = '\0';
        if (line[0] == '\0') continue;
        registry_add("CLIENT", line, "");
    }
    g_registry_loading = 0;
    fclose(fp);
}

// Get first SS entry
const char *registry_get_first_ss(void) {
    static char selected_ss[64] = {0};
    pthread_mutex_lock(&g_registry_mu);
    RegistryEntry *entry = g_registry_head;
    while (entry) {
        if (strcmp(entry->role, "SS") == 0) {
            size_t username_len = strlen(entry->username);
            size_t copy_len = (username_len < sizeof(selected_ss) - 1) ? username_len : sizeof(selected_ss) - 1;
            memcpy(selected_ss, entry->username, copy_len);
            selected_ss[copy_len] = '\0';
            pthread_mutex_unlock(&g_registry_mu);
            return selected_ss;
        }
        entry = entry->next;
    }
    pthread_mutex_unlock(&g_registry_mu);
    return NULL;
}

const char *registry_get_least_loaded_ss(void) {
    static char selected_ss[64] = {0};
    pthread_mutex_lock(&g_registry_mu);
    RegistryEntry *entry = g_registry_head;
    int best_count = -1;
    const char *best_username = NULL;
    while (entry) {
        if (strcmp(entry->role, "SS") == 0) {
            if (best_username == NULL || entry->file_count < best_count) {
                best_username = entry->username;
                best_count = entry->file_count;
            }
        }
        entry = entry->next;
    }
    if (best_username) {
        strncpy(selected_ss, best_username, sizeof(selected_ss) - 1);
        selected_ss[sizeof(selected_ss) - 1] = '\0';
        pthread_mutex_unlock(&g_registry_mu);
        return selected_ss;
    }
    pthread_mutex_unlock(&g_registry_mu);
    return NULL;
}

// Get SS info by username
int registry_get_ss_info(const char *ss_username, char *host, size_t host_len,
                         int *client_port) {
    if (!ss_username || !host || !client_port) return -1;
    
    pthread_mutex_lock(&g_registry_mu);
    RegistryEntry *entry = g_registry_head;
    while (entry) {
        if (strcmp(entry->role, "SS") == 0 &&
            strcmp(entry->username, ss_username) == 0) {
            // Parse payload
            char *host_start = strstr(entry->payload, "host=");
            char *port_start = strstr(entry->payload, "client_port=");
            
            if (host_start) {
                char *host_end = strchr(host_start + 5, ',');
                if (host_end) {
                    size_t host_str_len = host_end - (host_start + 5);
                    size_t copy_len = (host_str_len < host_len - 1) ? host_str_len : host_len - 1;
                    memcpy(host, host_start + 5, copy_len);
                    host[copy_len] = '\0';
                }
            }
            
            if (port_start) {
                *client_port = atoi(port_start + 12);
            }
            
            pthread_mutex_unlock(&g_registry_mu);
            return 0;
        }
        entry = entry->next;
    }
    pthread_mutex_unlock(&g_registry_mu);
    return -1;
}

// Get all client usernames
int registry_get_clients(char clients[][64], int max_clients) {
    int count = 0;
    pthread_mutex_lock(&g_registry_mu);
    RegistryEntry *entry = g_registry_head;
    while (entry && count < max_clients) {
        if (strcmp(entry->role, "CLIENT") == 0) {
            size_t username_len = strlen(entry->username);
            size_t copy_len = (username_len < 63) ? username_len : 63;
            memcpy(clients[count], entry->username, copy_len);
            clients[count][copy_len] = '\0';
            count++;
        }
        entry = entry->next;
    }
    pthread_mutex_unlock(&g_registry_mu);
    return count;
}

int registry_get_ss_candidates(char usernames[][64], int max_entries) {
    if (!usernames || max_entries <= 0) return 0;

    pthread_mutex_lock(&g_registry_mu);
    int ss_count = 0;
    RegistryEntry *entry = g_registry_head;
    while (entry) {
        if (strcmp(entry->role, "SS") == 0) {
            ss_count++;
        }
        entry = entry->next;
    }

    if (ss_count == 0) {
        pthread_mutex_unlock(&g_registry_mu);
        return 0;
    }

    SsCandidate *candidates = (SsCandidate*)calloc(ss_count, sizeof(SsCandidate));
    if (!candidates) {
        pthread_mutex_unlock(&g_registry_mu);
        return 0;
    }

    entry = g_registry_head;
    int idx = 0;
    while (entry) {
        if (strcmp(entry->role, "SS") == 0 && idx < ss_count) {
            // Skip backup servers (ending in "_backup") - they shouldn't be candidates for new files
            size_t username_len = strlen(entry->username);
            int is_backup = (username_len >= 7 && strcmp(entry->username + username_len - 7, "_backup") == 0);
            if (!is_backup) {
                strncpy(candidates[idx].username, entry->username, sizeof(candidates[idx].username) - 1);
                candidates[idx].username[sizeof(candidates[idx].username) - 1] = '\0';
                candidates[idx].file_count = entry->file_count;
                idx++;
            }
        }
        entry = entry->next;
    }
    pthread_mutex_unlock(&g_registry_mu);

    qsort(candidates, idx, sizeof(SsCandidate), compare_ss_candidates);

    int copy_count = (idx < max_entries) ? idx : max_entries;
    for (int i = 0; i < copy_count; i++) {
        strncpy(usernames[i], candidates[i].username, 63);
        usernames[i][63] = '\0';
    }

    free(candidates);
    return copy_count;
}

void registry_set_ss_file_count(const char *ss_username, int count) {
    pthread_mutex_lock(&g_registry_mu);
    RegistryEntry *entry = g_registry_head;
    while (entry) {
        if (strcmp(entry->role, "SS") == 0 &&
            strcmp(entry->username, ss_username) == 0) {
            entry->file_count = count;
            break;
        }
        entry = entry->next;
    }
    pthread_mutex_unlock(&g_registry_mu);
}

void registry_adjust_ss_file_count(const char *ss_username, int delta) {
    pthread_mutex_lock(&g_registry_mu);
    RegistryEntry *entry = g_registry_head;
    while (entry) {
        if (strcmp(entry->role, "SS") == 0 &&
            strcmp(entry->username, ss_username) == 0) {
            entry->file_count += delta;
            if (entry->file_count < 0) entry->file_count = 0;
            break;
        }
        entry = entry->next;
    }
    pthread_mutex_unlock(&g_registry_mu);
}

