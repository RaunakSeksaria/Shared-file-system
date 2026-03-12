#define _POSIX_C_SOURCE 200809L  // For strdup
#include "acl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Initialize ACL with owner
ACL acl_init(const char *owner) {
    ACL acl = {0};
    if (owner) {
        strncpy(acl.owner, owner, sizeof(acl.owner) - 1);
    }
    acl.count = 0;
    return acl;
}

// Find ACL entry for a user (internal helper)
static ACLEntry *acl_find_entry(ACL *acl, const char *username) {
    if (!acl || !username) return NULL;
    
    for (int i = 0; i < acl->count; i++) {
        if (strcmp(acl->entries[i].username, username) == 0) {
            return &acl->entries[i];
        }
    }
    return NULL;
}

// Grant read access to a user
int acl_add_read(ACL *acl, const char *username) {
    if (!acl || !username) return -1;
    
    // Cannot modify owner (owner always has RW)
    if (strcmp(acl->owner, username) == 0) {
        return 0;  // Owner already has read access
    }
    
    // Check if entry already exists
    ACLEntry *entry = acl_find_entry(acl, username);
    if (entry) {
        // Entry exists - grant read access
        entry->read_access = 1;
        return 0;
    }
    
    // Check if ACL is full
    if (acl->count >= MAX_ACL_ENTRIES) {
        return -1;  // ACL full
    }
    
    // Create new entry
    ACLEntry *new_entry = &acl->entries[acl->count];
    strncpy(new_entry->username, username, sizeof(new_entry->username) - 1);
    new_entry->read_access = 1;
    new_entry->write_access = 0;
    acl->count++;
    
    return 0;
}

// Grant write access to a user
int acl_add_write(ACL *acl, const char *username) {
    if (!acl || !username) return -1;
    
    // Cannot modify owner (owner always has RW)
    if (strcmp(acl->owner, username) == 0) {
        return 0;  // Owner already has write access
    }
    
    // Check if entry already exists
    ACLEntry *entry = acl_find_entry(acl, username);
    if (entry) {
        // Entry exists - grant write access (which includes read)
        entry->write_access = 1;
        entry->read_access = 1;  // Write implies read
        return 0;
    }
    
    // Check if ACL is full
    if (acl->count >= MAX_ACL_ENTRIES) {
        return -1;  // ACL full
    }
    
    // Create new entry with write access
    ACLEntry *new_entry = &acl->entries[acl->count];
    strncpy(new_entry->username, username, sizeof(new_entry->username) - 1);
    new_entry->read_access = 1;   // Write implies read
    new_entry->write_access = 1;
    acl->count++;
    
    return 0;
}

// Remove all access for a user
int acl_remove(ACL *acl, const char *username) {
    if (!acl || !username) return -1;
    
    // Cannot remove owner
    if (strcmp(acl->owner, username) == 0) {
        return -1;  // Cannot remove owner
    }
    
    // Find entry
    for (int i = 0; i < acl->count; i++) {
        if (strcmp(acl->entries[i].username, username) == 0) {
            // Found - remove by shifting remaining entries
            for (int j = i; j < acl->count - 1; j++) {
                acl->entries[j] = acl->entries[j + 1];
            }
            acl->count--;
            return 0;
        }
    }
    
    return -1;  // User not found
}

// Check if user has read access
int acl_check_read(const ACL *acl, const char *username) {
    if (!acl || !username) return 0;
    
    // Owner always has read access
    if (strcmp(acl->owner, username) == 0) {
        return 1;
    }
    
    // Check entries
    for (int i = 0; i < acl->count; i++) {
        if (strcmp(acl->entries[i].username, username) == 0) {
            return acl->entries[i].read_access;
        }
    }
    
    return 0;  // No access
}

// Check if user has write access
int acl_check_write(const ACL *acl, const char *username) {
    if (!acl || !username) return 0;
    
    // Owner always has write access
    if (strcmp(acl->owner, username) == 0) {
        return 1;
    }
    
    // Check entries
    for (int i = 0; i < acl->count; i++) {
        if (strcmp(acl->entries[i].username, username) == 0) {
            return acl->entries[i].write_access;
        }
    }
    
    return 0;  // No access
}

// Check if user is the owner
int acl_is_owner(const ACL *acl, const char *username) {
    if (!acl || !username) return 0;
    return (strcmp(acl->owner, username) == 0) ? 1 : 0;
}

// Serialize ACL to string format
int acl_serialize(const ACL *acl, char *buf, size_t buflen) {
    if (!acl || !buf || buflen == 0) return -1;
    
    int pos = 0;
    
    // Write owner
    int n = snprintf(buf + pos, buflen - pos, "owner=%s\n", acl->owner);
    if (n < 0 || (size_t)(pos + n) >= buflen) return -1;
    pos += n;
    
    // Write entries
    for (int i = 0; i < acl->count; i++) {
        const ACLEntry *entry = &acl->entries[i];
        const char *perm = entry->write_access ? "RW" : "R";
        n = snprintf(buf + pos, buflen - pos, "%s=%s\n", entry->username, perm);
        if (n < 0 || (size_t)(pos + n) >= buflen) return -1;
        pos += n;
    }
    
    return 0;
}

// Deserialize ACL from string format
int acl_deserialize(ACL *acl, const char *buf) {
    if (!acl || !buf) return -1;
    
    // Initialize ACL
    memset(acl, 0, sizeof(ACL));
    
    // Parse line by line
    char *tmp = strdup(buf);  // Make copy for strtok
    if (!tmp) return -1;
    
    char *line = strtok(tmp, "\n");
    while (line) {
        // Parse owner=username
        if (strncmp(line, "owner=", 6) == 0) {
            strncpy(acl->owner, line + 6, sizeof(acl->owner) - 1);
        }
        // Parse username=R or username=RW
        else {
            char *eq = strchr(line, '=');
            if (eq && acl->count < MAX_ACL_ENTRIES) {
                *eq = '\0';
                const char *username = line;
                const char *perm = eq + 1;
                
                ACLEntry *entry = &acl->entries[acl->count];
                strncpy(entry->username, username, sizeof(entry->username) - 1);
                
                if (strcmp(perm, "RW") == 0) {
                    entry->read_access = 1;
                    entry->write_access = 1;
                } else if (strcmp(perm, "R") == 0) {
                    entry->read_access = 1;
                    entry->write_access = 0;
                }
                
                acl->count++;
            }
        }
        line = strtok(NULL, "\n");
    }
    
    free(tmp);
    return 0;
}

