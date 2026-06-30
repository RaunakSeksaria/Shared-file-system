#ifndef ACCESS_REQUESTS_H
#define ACCESS_REQUESTS_H

#include <time.h>

// Maximum filename and path lengths
#define MAX_FILENAME_LEN 256
#define MAX_FOLDER_PATH_LEN 512
#define MAX_USERNAME_LEN 64

// Access request structure
typedef struct AccessRequest {
    char filename[MAX_FILENAME_LEN];        // Base filename (e.g., "report.txt")
    char folder_path[MAX_FOLDER_PATH_LEN];  // Folder path (e.g., "/documents/")
    char requester[MAX_USERNAME_LEN];       // Username requesting access
    char owner[MAX_USERNAME_LEN];           // File owner at time of request
    char access_type;                        // 'R' for read, 'W' for write, 'B' for both (RW)
    time_t requested_at;                     // Timestamp
    int request_id;                          // Unique ID
    struct AccessRequest *next;              // Linked list
} AccessRequest;

// Access request queue (linked list)
typedef struct {
    AccessRequest *head;
    int next_id;  // Counter for generating unique IDs
    int count;    // Total number of requests
} AccessRequestQueue;

// Initialize the access request queue
void request_queue_init(void);

// Add a new access request
// Returns: request_id on success, -1 on error, -2 if duplicate exists
int request_queue_add(const char *filename, const char *folder_path, 
                      const char *requester, const char *owner, char access_type);

// Remove a request by ID
// Returns: 0 on success, -1 on error (not found)
int request_queue_remove(int request_id);

// Get all requests for files owned by a specific user
// Returns: array of AccessRequest pointers, count stored in *count_out
// Caller must free the returned array (but NOT the request objects)
AccessRequest **request_queue_get_by_owner(const char *owner, int *count_out);

// Get all requests for files owned by a user with optional filename filter
// If filename is NULL, returns all requests for the owner
// Returns: array of AccessRequest pointers, count stored in *count_out
AccessRequest **request_queue_get_by_owner_filtered(const char *owner, 
                                                     const char *filename,
                                                     const char *folder_path,
                                                     int *count_out);

// Get a request by ID
// Returns: pointer to request, or NULL if not found
AccessRequest *request_queue_get_by_id(int request_id);

// Check if a duplicate request exists (same file, same requester)
// Returns: 1 if duplicate exists, 0 otherwise
int request_queue_has_duplicate(const char *filename, const char *folder_path,
                                 const char *requester);

// Update filename and folder_path for all requests when a file is moved
// Called by MOVE command
void request_queue_update_filename(const char *old_filename, const char *old_folder_path,
                                   const char *new_filename, const char *new_folder_path);

// Remove all requests for a specific file
// Called by DELETE command
void request_queue_remove_by_filename(const char *filename, const char *folder_path);

// Cleanup and free all resources
void request_queue_destroy(void);

#endif // ACCESS_REQUESTS_H
