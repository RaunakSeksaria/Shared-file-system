#ifndef ACCESS_CONTROL_H
#define ACCESS_CONTROL_H

#include "../common/acl.h"
#include "../common/errors.h"

// Access Control Enforcement Module for Name Server
// Provides functions to check file access permissions before operations

// Check if user can access a file (read or write)
// filename: Name of the file
// username: Username requesting access
// need_write: 1 if write access needed, 0 if read access
// acl: Pointer to ACL structure (populated from file metadata)
// Returns: Error structure (ERR_OK if allowed, ERR_UNAUTHORIZED if denied)
//
// This function:
// 1. Loads ACL from file metadata (or uses cached ACL)
// 2. Checks if user is owner (owner always has RW)
// 3. Checks if user has required permission (read or write)
// 4. Returns appropriate error code
//
// Usage:
//   ACL acl;
//   // Load acl from file metadata...
//   Error err = check_file_access("test.txt", "bob", 0, &acl);
//   if (error_is_ok(&err)) {
//       printf("Access granted\n");
//   }
Error check_file_access(const char *filename, const char *username,
                        int need_write, const ACL *acl);

// Check if user is the file owner
// filename: Name of the file
// username: Username to check
// acl: Pointer to ACL structure
// Returns: Error structure (ERR_OK if owner, ERR_UNAUTHORIZED if not)
//
// Used for operations that require ownership (DELETE, ADDACCESS, REMACCESS)
//
// Usage:
//   Error err = check_file_owner("test.txt", "alice", &acl);
Error check_file_owner(const char *filename, const char *username, const ACL *acl);

#endif

