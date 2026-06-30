#ifndef REGISTRY_H
#define REGISTRY_H

#include <stddef.h>

// Registry for NM to track SS and Client registrations
// Provides thread-safe access to registered components

// Registry entry structure
typedef struct RegistryEntry {
    char role[16];      // "SS" or "CLIENT"
    char username[64];
    char payload[256];  // Additional info (host, port, etc.)
    int file_count;
    struct RegistryEntry *next;
} RegistryEntry;

// Initialize on-disk persistence for client usernames (optional)
void registry_init_persistence(const char *path);

// Add entry to registry. Returns 1 if a new entry was inserted, 0 if updated.
int registry_add(const char *role, const char *username, const char *payload);

// Get first SS entry (for round-robin selection)
// Returns username of first SS, or NULL if none
const char *registry_get_first_ss(void);
const char *registry_get_least_loaded_ss(void);

// Get SS info (host and client_port) by username
// Returns 0 on success, -1 if SS not found
int registry_get_ss_info(const char *ss_username, char *host, size_t host_len,
                         int *client_port);

// Get all client usernames
// Fills provided array and returns count
int registry_get_clients(char clients[][64], int max_clients);

// Retrieve SS usernames sorted by ascending file count (lower first)
int registry_get_ss_candidates(char usernames[][64], int max_entries);

void registry_set_ss_file_count(const char *ss_username, int count);
void registry_adjust_ss_file_count(const char *ss_username, int delta);

#endif

