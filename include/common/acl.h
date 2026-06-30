#ifndef ACL_H
#define ACL_H

#include <stddef.h>

// Access Control List (ACL) Module
// Manages file permissions: owner, read access, write access
// ACLs are stored on SS (in metadata) and cached on NM for fast checks

// Maximum username length
#define MAX_USERNAME 64
#define MAX_ACL_ENTRIES 100  // Maximum number of users with access per file

// Single ACL entry: permissions for one user
typedef struct ACLEntry {
    char username[MAX_USERNAME];  // Username
    int read_access;               // 1 = has read access, 0 = no read access
    int write_access;              // 1 = has write access, 0 = no write access
} ACLEntry;

// Complete ACL for a file
// Contains owner and list of users with access
typedef struct ACL {
    char owner[MAX_USERNAME];           // File owner (always has RW access)
    ACLEntry entries[MAX_ACL_ENTRIES];  // Array of access entries
    int count;                           // Number of entries (users with access)
} ACL;

// Initialize ACL with owner
// owner: Username of file owner
// Returns: ACL structure with owner set
//
// The owner always has read and write access (this is implicit)
// This function creates a new ACL with just the owner
//
// Usage:
//   ACL acl = acl_init("alice");
ACL acl_init(const char *owner);

// Grant read access to a user
// acl: ACL to modify
// username: Username to grant read access
// Returns: 0 on success, -1 on error (ACL full, invalid username, etc.)
//
// If user already has read access, does nothing
// If user has write access, read access is already granted (implied)
//
// Usage:
//   ACL acl = acl_init("alice");
//   acl_add_read(&acl, "bob");  // Bob can now read
int acl_add_read(ACL *acl, const char *username);

// Grant write access to a user
// acl: ACL to modify
// username: Username to grant write access
// Returns: 0 on success, -1 on error
//
// Write access automatically includes read access
// If user already has write access, does nothing
//
// Usage:
//   acl_add_write(&acl, "bob");  // Bob can now read and write
int acl_add_write(ACL *acl, const char *username);

// Remove all access for a user
// acl: ACL to modify
// username: Username to remove access from
// Returns: 0 on success, -1 if user not found
//
// Cannot remove owner's access (owner always has RW)
// This removes the user from the ACL entirely
//
// Usage:
//   acl_remove(&acl, "bob");  // Bob no longer has access
int acl_remove(ACL *acl, const char *username);

// Check if user has read access
// acl: ACL to check
// username: Username to check
// Returns: 1 if user has read access, 0 otherwise
//
// Owner always has read access (even if not explicitly in entries)
// Checks both explicit entries and owner
//
// Usage:
//   if (acl_check_read(&acl, "bob")) {
//       printf("Bob can read\n");
//   }
int acl_check_read(const ACL *acl, const char *username);

// Check if user has write access
// acl: ACL to check
// username: Username to check
// Returns: 1 if user has write access, 0 otherwise
//
// Owner always has write access (even if not explicitly in entries)
// Checks both explicit entries and owner
//
// Usage:
//   if (acl_check_write(&acl, "bob")) {
//       printf("Bob can write\n");
//   }
int acl_check_write(const ACL *acl, const char *username);

// Check if user is the owner
// acl: ACL to check
// username: Username to check
// Returns: 1 if user is owner, 0 otherwise
//
// Usage:
//   if (acl_is_owner(&acl, "alice")) {
//       printf("Alice is the owner\n");
//   }
int acl_is_owner(const ACL *acl, const char *username);

// Serialize ACL to string format for storage
// acl: ACL to serialize
// buf: Buffer to write serialized ACL to
// buflen: Size of buffer
// Returns: 0 on success, -1 on error
//
// Format: "owner=username\nuser1=R\nuser2=RW\n"
// R = read only, RW = read and write
//
// Usage:
//   char buf[4096];
//   acl_serialize(&acl, buf, sizeof(buf));
int acl_serialize(const ACL *acl, char *buf, size_t buflen);

// Deserialize ACL from string format
// acl: ACL structure to populate
// buf: Serialized ACL string
// Returns: 0 on success, -1 on error
//
// Parses format: "owner=username\nuser1=R\nuser2=RW\n"
// Reconstructs ACL structure from string
//
// Usage:
//   ACL acl;
//   if (acl_deserialize(&acl, metadata_string) == 0) {
//       // ACL loaded successfully
//   }
int acl_deserialize(ACL *acl, const char *buf);

#endif

