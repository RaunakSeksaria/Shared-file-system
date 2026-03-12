#include "access_control.h"

#include <string.h>

// Check if user can access a file
Error check_file_access(const char *filename, const char *username,
                        int need_write, const ACL *acl) {
    if (!filename || !username || !acl) {
        return error_simple(ERR_INVALID, "Invalid parameters");
    }
    
    // Check if user is owner (owner always has RW access)
    if (acl_is_owner(acl, username)) {
        return error_ok();
    }
    
    // Check permissions
    if (need_write) {
        // Need write access
        if (acl_check_write(acl, username)) {
            return error_ok();
        } else {
            return error_create(ERR_UNAUTHORIZED, 
                               "User '%s' does not have write access to file '%s'",
                               username, filename);
        }
    } else {
        // Need read access
        if (acl_check_read(acl, username)) {
            return error_ok();
        } else {
            return error_create(ERR_UNAUTHORIZED,
                               "User '%s' does not have read access to file '%s'",
                               username, filename);
        }
    }
}

// Check if user is the file owner
Error check_file_owner(const char *filename, const char *username, const ACL *acl) {
    if (!filename || !username || !acl) {
        return error_simple(ERR_INVALID, "Invalid parameters");
    }
    
    if (acl_is_owner(acl, username)) {
        return error_ok();
    } else {
        return error_create(ERR_UNAUTHORIZED,
                           "User '%s' is not the owner of file '%s'",
                           username, filename);
    }
}

