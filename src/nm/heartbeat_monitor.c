#define _POSIX_C_SOURCE 200809L
#include "heartbeat_monitor.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../common/log.h"

// Global state
static HeartbeatStatus *g_heartbeat_list = NULL;
static pthread_mutex_t g_heartbeat_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_monitor_thread;
static volatile int g_monitor_running = 0;
static FailureCallback g_failure_callback = NULL;

// Initialize the heartbeat monitoring system
void heartbeat_monitor_init(void) {
    pthread_mutex_lock(&g_heartbeat_mu);
    
    // Clear existing list
    HeartbeatStatus *current = g_heartbeat_list;
    while (current) {
        HeartbeatStatus *next = current->next;
        free(current);
        current = next;
    }
    
    g_heartbeat_list = NULL;
    g_monitor_running = 0;
    g_failure_callback = NULL;
    
    pthread_mutex_unlock(&g_heartbeat_mu);
    
    log_info("heartbeat_monitor_init", "Heartbeat monitoring system initialized");
}

// Register a new SS for monitoring
void heartbeat_monitor_register_ss(const char *ss_username) {
    if (!ss_username) return;
    
    pthread_mutex_lock(&g_heartbeat_mu);
    
    // Check if already registered
    HeartbeatStatus *current = g_heartbeat_list;
    while (current) {
        if (strcmp(current->ss_username, ss_username) == 0) {
            // SS re-registering - update status
            current->last_heartbeat = time(NULL);
            current->status = SS_STATUS_ALIVE;
            current->missed_count = 0;
            pthread_mutex_unlock(&g_heartbeat_mu);
            log_info("heartbeat_monitor_register", "SS re-registered: %s", ss_username);
            return;
        }
        current = current->next;
    }
    
    // Create new entry
    HeartbeatStatus *entry = (HeartbeatStatus *)calloc(1, sizeof(HeartbeatStatus));
    if (!entry) {
        pthread_mutex_unlock(&g_heartbeat_mu);
        log_error("heartbeat_monitor_register", "Failed to allocate memory for %s", ss_username);
        return;
    }
    
    strncpy(entry->ss_username, ss_username, sizeof(entry->ss_username) - 1);
    entry->last_heartbeat = time(NULL);
    entry->first_seen = time(NULL);
    entry->status = SS_STATUS_ALIVE;
    entry->missed_count = 0;
    entry->next = g_heartbeat_list;
    g_heartbeat_list = entry;
    
    pthread_mutex_unlock(&g_heartbeat_mu);
    
    log_info("heartbeat_monitor_register", "SS registered for monitoring: %s", ss_username);
}

// Update heartbeat timestamp for an SS
void heartbeat_monitor_update(const char *ss_username) {
    if (!ss_username) return;
    
    pthread_mutex_lock(&g_heartbeat_mu);
    
    HeartbeatStatus *current = g_heartbeat_list;
    while (current) {
        if (strcmp(current->ss_username, ss_username) == 0) {
            time_t now = time(NULL);
            time_t prev = current->last_heartbeat;
            
            current->last_heartbeat = now;
            current->missed_count = 0;
            
            // If SS was failed, mark as recovered
            if (current->status == SS_STATUS_FAILED) {
                current->status = SS_STATUS_ALIVE;
                log_info("heartbeat_monitor_update", "SS recovered: %s (was down for %ld seconds)", 
                         ss_username, (long)(now - prev));
            }
            
            pthread_mutex_unlock(&g_heartbeat_mu);
            return;
        }
        current = current->next;
    }
    
    // SS not found - register it
    pthread_mutex_unlock(&g_heartbeat_mu);
    heartbeat_monitor_register_ss(ss_username);
}

// Monitoring thread function
static void *monitor_thread_func(void *arg) {
    (void)arg;
    
    log_info("heartbeat_monitor_thread", "Monitoring thread started");
    
    while (g_monitor_running) {
        sleep(HEARTBEAT_CHECK_INTERVAL);
        
        if (!g_monitor_running) break;
        
        pthread_mutex_lock(&g_heartbeat_mu);
        
        time_t now = time(NULL);
        HeartbeatStatus *current = g_heartbeat_list;
        
        while (current) {
            if (current->status == SS_STATUS_ALIVE) {
                time_t elapsed = now - current->last_heartbeat;
                
                // Check if heartbeat is overdue
                if (elapsed > HEARTBEAT_TIMEOUT_SEC) {
                    current->missed_count++;
                    
                    log_warning("heartbeat_monitor_check", 
                               "SS %s missed heartbeat (count=%d, last_seen=%ld seconds ago)",
                               current->ss_username, current->missed_count, (long)elapsed);
                    
                    // Mark as failed if threshold reached
                    if (current->missed_count >= MAX_MISSED_HEARTBEATS) {
                        current->status = SS_STATUS_FAILED;
                        
                        log_error("heartbeat_monitor_failure", 
                                 "SS %s marked as FAILED (timeout=%ld seconds, missed=%d heartbeats)",
                                 current->ss_username, (long)elapsed, current->missed_count);
                        
                        // Notify callback
                        if (g_failure_callback) {
                            // Call callback outside of lock to avoid deadlock
                            char username[64];
                            strncpy(username, current->ss_username, sizeof(username) - 1);
                            username[sizeof(username) - 1] = '\0';
                            
                            pthread_mutex_unlock(&g_heartbeat_mu);
                            g_failure_callback(username);
                            pthread_mutex_lock(&g_heartbeat_mu);
                            
                            // Re-find current in case list changed
                            HeartbeatStatus *temp = g_heartbeat_list;
                            while (temp && strcmp(temp->ss_username, username) != 0) {
                                temp = temp->next;
                            }
                            if (!temp) {
                                // Entry was removed, restart from head
                                current = g_heartbeat_list;
                                continue;
                            }
                            current = temp;
                        }
                    }
                }
            }
            
            current = current->next;
        }
        
        pthread_mutex_unlock(&g_heartbeat_mu);
    }
    
    log_info("heartbeat_monitor_thread", "Monitoring thread stopped");
    return NULL;
}

// Start the monitoring thread
int heartbeat_monitor_start(void) {
    if (g_monitor_running) {
        log_warning("heartbeat_monitor_start", "Monitoring thread already running");
        return 0;
    }
    
    g_monitor_running = 1;
    
    int rc = pthread_create(&g_monitor_thread, NULL, monitor_thread_func, NULL);
    if (rc != 0) {
        g_monitor_running = 0;
        log_error("heartbeat_monitor_start", "Failed to create monitoring thread: %d", rc);
        return -1;
    }
    
    log_info("heartbeat_monitor_start", "Heartbeat monitoring started (timeout=%d sec, check_interval=%d sec)",
             HEARTBEAT_TIMEOUT_SEC, HEARTBEAT_CHECK_INTERVAL);
    
    return 0;
}

// Stop the monitoring thread
void heartbeat_monitor_stop(void) {
    if (!g_monitor_running) return;
    
    log_info("heartbeat_monitor_stop", "Stopping monitoring thread...");
    
    g_monitor_running = 0;
    
    // Wait for thread to finish
    pthread_join(g_monitor_thread, NULL);
    
    log_info("heartbeat_monitor_stop", "Monitoring thread stopped");
}

// Set callback function for failure notifications
void heartbeat_monitor_set_failure_callback(FailureCallback callback) {
    pthread_mutex_lock(&g_heartbeat_mu);
    g_failure_callback = callback;
    pthread_mutex_unlock(&g_heartbeat_mu);
    
    if (callback) {
        log_info("heartbeat_monitor_callback", "Failure callback registered");
    }
}

// Get current status of an SS
SSStatus heartbeat_monitor_get_status(const char *ss_username) {
    if (!ss_username) return SS_STATUS_UNKNOWN;
    
    pthread_mutex_lock(&g_heartbeat_mu);
    
    HeartbeatStatus *current = g_heartbeat_list;
    while (current) {
        if (strcmp(current->ss_username, ss_username) == 0) {
            SSStatus status = current->status;
            pthread_mutex_unlock(&g_heartbeat_mu);
            return status;
        }
        current = current->next;
    }
    
    pthread_mutex_unlock(&g_heartbeat_mu);
    return SS_STATUS_UNKNOWN;
}

// Check if SS is alive
int heartbeat_monitor_is_alive(const char *ss_username) {
    SSStatus status = heartbeat_monitor_get_status(ss_username);
    return (status == SS_STATUS_ALIVE) ? 1 : 0;
}

// Manually mark an SS as failed
void heartbeat_monitor_mark_failed(const char *ss_username) {
    if (!ss_username) return;
    
    pthread_mutex_lock(&g_heartbeat_mu);
    
    HeartbeatStatus *current = g_heartbeat_list;
    while (current) {
        if (strcmp(current->ss_username, ss_username) == 0) {
            if (current->status != SS_STATUS_FAILED) {
                current->status = SS_STATUS_FAILED;
                current->missed_count = MAX_MISSED_HEARTBEATS;
                
                log_error("heartbeat_monitor_mark_failed", "SS manually marked as FAILED: %s", ss_username);
                
                // Notify callback
                if (g_failure_callback) {
                    char username[64];
                    strncpy(username, ss_username, sizeof(username) - 1);
                    username[sizeof(username) - 1] = '\0';
                    
                    pthread_mutex_unlock(&g_heartbeat_mu);
                    g_failure_callback(username);
                    return;
                }
            }
            pthread_mutex_unlock(&g_heartbeat_mu);
            return;
        }
        current = current->next;
    }
    
    pthread_mutex_unlock(&g_heartbeat_mu);
    log_warning("heartbeat_monitor_mark_failed", "SS not found: %s", ss_username);
}

// Get list of all failed SS
int heartbeat_monitor_get_failed_ss(char failed[][64], int max_entries) {
    if (!failed || max_entries <= 0) return 0;
    
    pthread_mutex_lock(&g_heartbeat_mu);
    
    int count = 0;
    HeartbeatStatus *current = g_heartbeat_list;
    
    while (current && count < max_entries) {
        if (current->status == SS_STATUS_FAILED) {
            size_t len = strlen(current->ss_username);
            size_t copy_len = (len < 63) ? len : 63;
            memcpy(failed[count], current->ss_username, copy_len);
            failed[count][copy_len] = '\0';
            count++;
        }
        current = current->next;
    }
    
    pthread_mutex_unlock(&g_heartbeat_mu);
    return count;
}
