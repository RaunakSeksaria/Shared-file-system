#ifndef REPLICATION_H
#define REPLICATION_H

#include <time.h>

// Replication Management for Name Server
// Tracks primary-replica pairs and manages failover/recovery

#define MAX_SS_USERNAME 64
#define MAX_FILENAME 256

// Replication pair status
typedef enum {
    REPL_STATUS_SYNCED,      // Replica is up-to-date
    REPL_STATUS_SYNCING,     // Sync in progress
    REPL_STATUS_FAILED,      // Replica failed or unreachable
    REPL_STATUS_PRIMARY_FAILED  // Primary failed, replica promoted
} ReplicationStatus;

// Alias for clarity
typedef ReplicationStatus ReplicationPairStatus;

// Represents a primary-replica pair
typedef struct ReplicationPair {
    char primary_ss[MAX_SS_USERNAME];     // Primary SS username
    char replica_ss[MAX_SS_USERNAME];     // Replica SS username
    ReplicationStatus status;             // Current sync status
    time_t last_synced;                   // Last successful sync time
    int files_synced;                     // Number of files synced
    struct ReplicationPair *next;
} ReplicationPair;

// Initialize replication system
void replication_init(void);

// Assign replica for a primary SS
// Returns 0 on success, -1 if no suitable replica found
// Strategy: primary_ss → primary_ss_backup (e.g., ss1 → ss1_backup)
int replication_assign_replica(const char *primary_ss);

// Get replica SS for a primary
// Returns replica username, or NULL if not found
const char *replication_get_replica(const char *primary_ss);

// Get primary SS for a replica
// Returns primary username, or NULL if not found
const char *replication_get_primary(const char *replica_ss);

// Check if SS is a replica
// Returns 1 if SS is a replica, 0 otherwise
int replication_is_replica(const char *ss_username);

// Mark primary SS as failed and promote replica
// Returns 0 on success, -1 if no replica found
int replication_failover(const char *failed_primary);

// Handle recovery when failed primary reconnects
// Swaps roles back and triggers sync
// Returns 0 on success, -1 on error
int replication_recover(const char *recovered_ss);

// Get current primary for a logical SS (handles failover)
// E.g., if ss1 failed, returns ss1_backup
// Returns actual active primary username
const char *replication_get_active_primary(const char *logical_ss);

// Update sync timestamp for a pair
void replication_mark_synced(const char *primary_ss, const char *replica_ss);

// Get all replication pairs (for debugging/monitoring)
// Returns number of pairs
int replication_get_all_pairs(ReplicationPair *pairs, int max_pairs);

// Remove replication pair (when SS permanently removed)
void replication_remove_pair(const char *ss_username);

// Get pair status for an SS
ReplicationPairStatus replication_get_pair_status(const char *ss_username);

// Get primary for replica (alternative name for clarity)
const char *replication_get_primary_for_replica(const char *replica_ss);

#endif
