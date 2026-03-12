#define _POSIX_C_SOURCE 200809L
#include "replication.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/log.h"
#include "registry.h"

// Global state
static ReplicationPair *g_repl_pairs = NULL;
static pthread_mutex_t g_repl_mu = PTHREAD_MUTEX_INITIALIZER;

// Initialize replication system
void replication_init(void) {
    pthread_mutex_lock(&g_repl_mu);
    
    // Clear existing pairs
    ReplicationPair *current = g_repl_pairs;
    while (current) {
        ReplicationPair *next = current->next;
        free(current);
        current = next;
    }
    
    g_repl_pairs = NULL;
    
    pthread_mutex_unlock(&g_repl_mu);
    
    log_info("replication_init", "Replication system initialized");
}

// Find pair by primary or replica SS
static ReplicationPair *find_pair_by_ss(const char *ss_username) {
    ReplicationPair *current = g_repl_pairs;
    while (current) {
        if (strcmp(current->primary_ss, ss_username) == 0 ||
            strcmp(current->replica_ss, ss_username) == 0) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

// Assign replica for a primary SS
// Strategy: ss1 → ss1_backup, ss2 → ss2_backup, etc.
int replication_assign_replica(const char *primary_ss) {
    if (!primary_ss) return -1;
    
    pthread_mutex_lock(&g_repl_mu);
    
    // Check if already paired
    ReplicationPair *existing = find_pair_by_ss(primary_ss);
    if (existing) {
        pthread_mutex_unlock(&g_repl_mu);
        log_info("replication_assign", "SS %s already paired with %s", 
                 primary_ss, existing->replica_ss);
        return 0;
    }
    
    // Construct expected backup name: primary_ss + "_backup"
    char expected_backup[MAX_SS_USERNAME];
    snprintf(expected_backup, sizeof(expected_backup), "%s_backup", primary_ss);
    
    // Check if backup SS exists in registry
    char backup_host[64];
    int backup_port;
    if (registry_get_ss_info(expected_backup, backup_host, sizeof(backup_host), 
                             &backup_port) != 0) {
        // Backup not found - this is OK, not all SS need backups
        pthread_mutex_unlock(&g_repl_mu);
        log_info("replication_assign", "No backup found for %s (expected: %s)", 
                 primary_ss, expected_backup);
        return -1;
    }
    
    // Create new pair
    ReplicationPair *pair = (ReplicationPair *)calloc(1, sizeof(ReplicationPair));
    if (!pair) {
        pthread_mutex_unlock(&g_repl_mu);
        log_error("replication_assign", "Failed to allocate pair for %s", primary_ss);
        return -1;
    }
    
    size_t primary_len = strlen(primary_ss);
    size_t primary_copy = (primary_len < sizeof(pair->primary_ss) - 1) ? primary_len : sizeof(pair->primary_ss) - 1;
    memcpy(pair->primary_ss, primary_ss, primary_copy);
    pair->primary_ss[primary_copy] = '\0';
    
    size_t backup_len = strlen(expected_backup);
    size_t backup_copy = (backup_len < sizeof(pair->replica_ss) - 1) ? backup_len : sizeof(pair->replica_ss) - 1;
    memcpy(pair->replica_ss, expected_backup, backup_copy);
    pair->replica_ss[backup_copy] = '\0';
    
    pair->status = REPL_STATUS_SYNCING;  // Will sync after creation
    pair->last_synced = 0;
    pair->files_synced = 0;
    pair->next = g_repl_pairs;
    g_repl_pairs = pair;
    
    pthread_mutex_unlock(&g_repl_mu);
    
    log_info("replication_assign", "Paired %s → %s", primary_ss, expected_backup);
    return 0;
}

// Get replica SS for a primary
const char *replication_get_replica(const char *primary_ss) {
    if (!primary_ss) return NULL;
    
    pthread_mutex_lock(&g_repl_mu);
    
    ReplicationPair *current = g_repl_pairs;
    while (current) {
        if (strcmp(current->primary_ss, primary_ss) == 0) {
            static char result[MAX_SS_USERNAME];
            strncpy(result, current->replica_ss, sizeof(result) - 1);
            result[sizeof(result) - 1] = '\0';
            pthread_mutex_unlock(&g_repl_mu);
            return result;
        }
        current = current->next;
    }
    
    pthread_mutex_unlock(&g_repl_mu);
    return NULL;
}

// Get primary SS for a replica
const char *replication_get_primary(const char *replica_ss) {
    if (!replica_ss) return NULL;
    
    pthread_mutex_lock(&g_repl_mu);
    
    ReplicationPair *current = g_repl_pairs;
    while (current) {
        if (strcmp(current->replica_ss, replica_ss) == 0) {
            static char result[MAX_SS_USERNAME];
            strncpy(result, current->primary_ss, sizeof(result) - 1);
            result[sizeof(result) - 1] = '\0';
            pthread_mutex_unlock(&g_repl_mu);
            return result;
        }
        current = current->next;
    }
    
    pthread_mutex_unlock(&g_repl_mu);
    return NULL;
}

// Check if SS is a replica
int replication_is_replica(const char *ss_username) {
    if (!ss_username) return 0;
    
    pthread_mutex_lock(&g_repl_mu);
    
    ReplicationPair *current = g_repl_pairs;
    while (current) {
        if (strcmp(current->replica_ss, ss_username) == 0) {
            pthread_mutex_unlock(&g_repl_mu);
            return 1;
        }
        current = current->next;
    }
    
    pthread_mutex_unlock(&g_repl_mu);
    return 0;
}

// Mark primary as failed and promote replica
int replication_failover(const char *failed_primary) {
    if (!failed_primary) return -1;
    
    pthread_mutex_lock(&g_repl_mu);
    
    ReplicationPair *pair = find_pair_by_ss(failed_primary);
    if (!pair) {
        pthread_mutex_unlock(&g_repl_mu);
        log_warning("replication_failover", "No pair found for failed SS: %s", failed_primary);
        return -1;
    }
    
    // Check which role failed
    if (strcmp(pair->primary_ss, failed_primary) == 0) {
        // Primary failed - promote replica
        pair->status = REPL_STATUS_PRIMARY_FAILED;
        log_error("replication_failover", "Primary %s failed, promoting replica %s", 
                 failed_primary, pair->replica_ss);
    } else {
        // Replica failed - mark as failed
        pair->status = REPL_STATUS_FAILED;
        log_error("replication_failover", "Replica %s failed, primary %s continues", 
                 failed_primary, pair->primary_ss);
    }
    
    pthread_mutex_unlock(&g_repl_mu);
    return 0;
}

// Handle recovery when failed SS reconnects
int replication_recover(const char *recovered_ss) {
    if (!recovered_ss) return -1;
    
    pthread_mutex_lock(&g_repl_mu);
    
    ReplicationPair *pair = find_pair_by_ss(recovered_ss);
    if (!pair) {
        pthread_mutex_unlock(&g_repl_mu);
        log_warning("replication_recover", "No pair found for recovered SS: %s", recovered_ss);
        return -1;
    }
    
    // Determine recovery scenario
    if (strcmp(pair->primary_ss, recovered_ss) == 0) {
        // Original primary recovered
        if (pair->status == REPL_STATUS_PRIMARY_FAILED) {
            // Replica was promoted, now swap back
            log_info("replication_recover", "Primary %s recovered, will sync from replica %s and resume", 
                     recovered_ss, pair->replica_ss);
            // Note: Actual sync happens in caller
            pair->status = REPL_STATUS_SYNCING;
        } else {
            log_info("replication_recover", "Primary %s recovered (replica was not promoted)", 
                     recovered_ss);
            pair->status = REPL_STATUS_SYNCED;
        }
    } else {
        // Replica recovered
        log_info("replication_recover", "Replica %s recovered, will re-sync from primary %s", 
                 recovered_ss, pair->primary_ss);
        pair->status = REPL_STATUS_SYNCING;
    }
    
    pthread_mutex_unlock(&g_repl_mu);
    return 0;
}

// Get active primary (handles failover)
const char *replication_get_active_primary(const char *logical_ss) {
    if (!logical_ss) return NULL;
    
    pthread_mutex_lock(&g_repl_mu);
    
    ReplicationPair *current = g_repl_pairs;
    while (current) {
        if (strcmp(current->primary_ss, logical_ss) == 0) {
            // Found pair where logical_ss is primary
            if (current->status == REPL_STATUS_PRIMARY_FAILED) {
                // Primary failed, return replica
                static char result[MAX_SS_USERNAME];
                strncpy(result, current->replica_ss, sizeof(result) - 1);
                result[sizeof(result) - 1] = '\0';
                pthread_mutex_unlock(&g_repl_mu);
                return result;
            } else {
                // Primary is active
                pthread_mutex_unlock(&g_repl_mu);
                return logical_ss;
            }
        }
        current = current->next;
    }
    
    pthread_mutex_unlock(&g_repl_mu);
    
    // No pair found, return original SS
    return logical_ss;
}

// Update sync timestamp
void replication_mark_synced(const char *primary_ss, const char *replica_ss) {
    if (!primary_ss || !replica_ss) return;
    
    pthread_mutex_lock(&g_repl_mu);
    
    ReplicationPair *current = g_repl_pairs;
    while (current) {
        if (strcmp(current->primary_ss, primary_ss) == 0 &&
            strcmp(current->replica_ss, replica_ss) == 0) {
            current->last_synced = time(NULL);
            current->files_synced++;
            if (current->status == REPL_STATUS_SYNCING) {
                current->status = REPL_STATUS_SYNCED;
            }
            pthread_mutex_unlock(&g_repl_mu);
            return;
        }
        current = current->next;
    }
    
    pthread_mutex_unlock(&g_repl_mu);
}

// Get all pairs (for monitoring)
int replication_get_all_pairs(ReplicationPair *pairs, int max_pairs) {
    if (!pairs || max_pairs <= 0) return 0;
    
    pthread_mutex_lock(&g_repl_mu);
    
    int count = 0;
    ReplicationPair *current = g_repl_pairs;
    
    while (current && count < max_pairs) {
        memcpy(&pairs[count], current, sizeof(ReplicationPair));
        count++;
        current = current->next;
    }
    
    pthread_mutex_unlock(&g_repl_mu);
    return count;
}

// Remove pair
void replication_remove_pair(const char *ss_username) {
    if (!ss_username) return;
    
    pthread_mutex_lock(&g_repl_mu);
    
    ReplicationPair *prev = NULL;
    ReplicationPair *current = g_repl_pairs;
    
    while (current) {
        if (strcmp(current->primary_ss, ss_username) == 0 ||
            strcmp(current->replica_ss, ss_username) == 0) {
            // Remove this pair
            if (prev) {
                prev->next = current->next;
            } else {
                g_repl_pairs = current->next;
            }
            
            log_info("replication_remove", "Removed pair involving %s", ss_username);
            free(current);
            pthread_mutex_unlock(&g_repl_mu);
            return;
        }
        
        prev = current;
        current = current->next;
    }
    
    pthread_mutex_unlock(&g_repl_mu);
}

// Get pair status for an SS (either primary or replica)
ReplicationPairStatus replication_get_pair_status(const char *ss_username) {
    if (!ss_username) return REPL_STATUS_SYNCED;
    
    pthread_mutex_lock(&g_repl_mu);
    
    ReplicationPair *pair = find_pair_by_ss(ss_username);
    if (pair) {
        ReplicationPairStatus status = pair->status;
        pthread_mutex_unlock(&g_repl_mu);
        return status;
    }
    
    pthread_mutex_unlock(&g_repl_mu);
    return REPL_STATUS_SYNCED;
}

// Get primary for a replica (wrapper function name for clarity)
const char *replication_get_primary_for_replica(const char *replica_ss) {
    return replication_get_primary(replica_ss);
}
