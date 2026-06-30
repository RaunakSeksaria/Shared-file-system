#ifndef HEARTBEAT_MONITOR_H
#define HEARTBEAT_MONITOR_H

#include <time.h>
#include <stddef.h>

// Heartbeat monitoring for Storage Servers
// Tracks last heartbeat time and detects failures

// Configuration
#define HEARTBEAT_TIMEOUT_SEC 15    // Time without heartbeat before marking as failed
#define HEARTBEAT_CHECK_INTERVAL 5  // How often to check for timeouts (seconds)
#define MAX_MISSED_HEARTBEATS 3     // Number of missed heartbeats before failure

// Status of a Storage Server
typedef enum {
    SS_STATUS_ALIVE,    // SS is healthy and sending heartbeats
    SS_STATUS_FAILED,   // SS has failed (timeout reached)
    SS_STATUS_UNKNOWN   // SS just registered, not yet confirmed
} SSStatus;

// Heartbeat status for a single SS
typedef struct HeartbeatStatus {
    char ss_username[64];           // SS identifier
    time_t last_heartbeat;          // Timestamp of last received heartbeat
    time_t first_seen;              // When SS was first registered
    SSStatus status;                // Current status
    int missed_count;               // Consecutive missed heartbeats
    struct HeartbeatStatus *next;   // Linked list
} HeartbeatStatus;

// Callback function type for failure notifications
// Called when an SS is marked as failed
typedef void (*FailureCallback)(const char *ss_username);

// Initialize heartbeat monitoring system
// Should be called once at NM startup
void heartbeat_monitor_init(void);

// Register a new SS for monitoring
// Call this when SS_REGISTER message is received
void heartbeat_monitor_register_ss(const char *ss_username);

// Update heartbeat timestamp for an SS
// Call this when HEARTBEAT message is received
void heartbeat_monitor_update(const char *ss_username);

// Start the monitoring thread
// Returns 0 on success, -1 on error
int heartbeat_monitor_start(void);

// Stop the monitoring thread
// Call during NM shutdown
void heartbeat_monitor_stop(void);

// Set callback function for failure notifications
void heartbeat_monitor_set_failure_callback(FailureCallback callback);

// Get current status of an SS
// Returns SS_STATUS_UNKNOWN if SS not found
SSStatus heartbeat_monitor_get_status(const char *ss_username);

// Check if SS is alive (for queries)
// Returns 1 if alive, 0 if failed/unknown
int heartbeat_monitor_is_alive(const char *ss_username);

// Manually mark an SS as failed (for testing or other reasons)
void heartbeat_monitor_mark_failed(const char *ss_username);

// Get list of all failed SS usernames
// Returns number of failed SS
int heartbeat_monitor_get_failed_ss(char failed[][64], int max_entries);

#endif
