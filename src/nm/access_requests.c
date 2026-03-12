#include "access_requests.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

// Global request queue
static AccessRequestQueue g_request_queue = {NULL, 1, 0};
static pthread_mutex_t g_queue_mutex = PTHREAD_MUTEX_INITIALIZER;

// Initialize the access request queue
void request_queue_init(void) {
    pthread_mutex_lock(&g_queue_mutex);
    g_request_queue.head = NULL;
    g_request_queue.next_id = 1;
    g_request_queue.count = 0;
    pthread_mutex_unlock(&g_queue_mutex);
}

// Add a new access request
int request_queue_add(const char *filename, const char *folder_path,
                      const char *requester, const char *owner, char access_type) {
    if (!filename || !folder_path || !requester || !owner) return -1;
    if (access_type != 'R' && access_type != 'W' && access_type != 'B') return -1;
    
    pthread_mutex_lock(&g_queue_mutex);
    
    // Check for duplicate
    if (request_queue_has_duplicate(filename, folder_path, requester)) {
        pthread_mutex_unlock(&g_queue_mutex);
        return -2;  // Duplicate exists
    }
    
    // Create new request
    AccessRequest *req = (AccessRequest *)calloc(1, sizeof(AccessRequest));
    if (!req) {
        pthread_mutex_unlock(&g_queue_mutex);
        return -1;
    }
    
    // Copy data
    strncpy(req->filename, filename, MAX_FILENAME_LEN - 1);
    req->filename[MAX_FILENAME_LEN - 1] = '\0';
    
    strncpy(req->folder_path, folder_path, MAX_FOLDER_PATH_LEN - 1);
    req->folder_path[MAX_FOLDER_PATH_LEN - 1] = '\0';
    
    strncpy(req->requester, requester, MAX_USERNAME_LEN - 1);
    req->requester[MAX_USERNAME_LEN - 1] = '\0';
    
    strncpy(req->owner, owner, MAX_USERNAME_LEN - 1);
    req->owner[MAX_USERNAME_LEN - 1] = '\0';
    
    req->access_type = access_type;
    req->requested_at = time(NULL);
    req->request_id = g_request_queue.next_id++;
    
    // Add to front of list
    req->next = g_request_queue.head;
    g_request_queue.head = req;
    g_request_queue.count++;
    
    int id = req->request_id;
    pthread_mutex_unlock(&g_queue_mutex);
    return id;
}

// Remove a request by ID
int request_queue_remove(int request_id) {
    pthread_mutex_lock(&g_queue_mutex);
    
    AccessRequest *curr = g_request_queue.head;
    AccessRequest *prev = NULL;
    
    while (curr) {
        if (curr->request_id == request_id) {
            // Found it - remove from list
            if (prev) {
                prev->next = curr->next;
            } else {
                g_request_queue.head = curr->next;
            }
            free(curr);
            g_request_queue.count--;
            pthread_mutex_unlock(&g_queue_mutex);
            return 0;
        }
        prev = curr;
        curr = curr->next;
    }
    
    pthread_mutex_unlock(&g_queue_mutex);
    return -1;  // Not found
}

// Get all requests for files owned by a specific user with optional filter
AccessRequest **request_queue_get_by_owner_filtered(const char *owner,
                                                     const char *filename,
                                                     const char *folder_path,
                                                     int *count_out) {
    if (!owner || !count_out) return NULL;
    
    pthread_mutex_lock(&g_queue_mutex);
    
    // First pass: count matching requests
    int count = 0;
    AccessRequest *curr = g_request_queue.head;
    while (curr) {
        int owner_match = (strcmp(curr->owner, owner) == 0);
        int file_match = 1;
        
        if (filename && folder_path) {
            file_match = (strcmp(curr->filename, filename) == 0 &&
                         strcmp(curr->folder_path, folder_path) == 0);
        }
        
        if (owner_match && file_match) {
            count++;
        }
        curr = curr->next;
    }
    
    if (count == 0) {
        *count_out = 0;
        pthread_mutex_unlock(&g_queue_mutex);
        return NULL;
    }
    
    // Allocate array
    AccessRequest **results = (AccessRequest **)malloc(count * sizeof(AccessRequest *));
    if (!results) {
        *count_out = 0;
        pthread_mutex_unlock(&g_queue_mutex);
        return NULL;
    }
    
    // Second pass: collect matching requests
    int idx = 0;
    curr = g_request_queue.head;
    while (curr && idx < count) {
        int owner_match = (strcmp(curr->owner, owner) == 0);
        int file_match = 1;
        
        if (filename && folder_path) {
            file_match = (strcmp(curr->filename, filename) == 0 &&
                         strcmp(curr->folder_path, folder_path) == 0);
        }
        
        if (owner_match && file_match) {
            results[idx++] = curr;
        }
        curr = curr->next;
    }
    
    *count_out = count;
    pthread_mutex_unlock(&g_queue_mutex);
    return results;
}

// Get all requests for files owned by a specific user
AccessRequest **request_queue_get_by_owner(const char *owner, int *count_out) {
    return request_queue_get_by_owner_filtered(owner, NULL, NULL, count_out);
}

// Get a request by ID
AccessRequest *request_queue_get_by_id(int request_id) {
    pthread_mutex_lock(&g_queue_mutex);
    
    AccessRequest *curr = g_request_queue.head;
    while (curr) {
        if (curr->request_id == request_id) {
            pthread_mutex_unlock(&g_queue_mutex);
            return curr;
        }
        curr = curr->next;
    }
    
    pthread_mutex_unlock(&g_queue_mutex);
    return NULL;
}

// Check if a duplicate request exists
int request_queue_has_duplicate(const char *filename, const char *folder_path,
                                 const char *requester) {
    // Note: assumes caller holds mutex
    AccessRequest *curr = g_request_queue.head;
    while (curr) {
        if (strcmp(curr->filename, filename) == 0 &&
            strcmp(curr->folder_path, folder_path) == 0 &&
            strcmp(curr->requester, requester) == 0) {
            return 1;  // Duplicate found
        }
        curr = curr->next;
    }
    return 0;  // No duplicate
}

// Update filename and folder_path for all requests when a file is moved
void request_queue_update_filename(const char *old_filename, const char *old_folder_path,
                                   const char *new_filename, const char *new_folder_path) {
    if (!old_filename || !old_folder_path || !new_filename || !new_folder_path) return;
    
    pthread_mutex_lock(&g_queue_mutex);
    
    AccessRequest *curr = g_request_queue.head;
    while (curr) {
        if (strcmp(curr->filename, old_filename) == 0 &&
            strcmp(curr->folder_path, old_folder_path) == 0) {
            // Update to new location
            strncpy(curr->filename, new_filename, MAX_FILENAME_LEN - 1);
            curr->filename[MAX_FILENAME_LEN - 1] = '\0';
            
            strncpy(curr->folder_path, new_folder_path, MAX_FOLDER_PATH_LEN - 1);
            curr->folder_path[MAX_FOLDER_PATH_LEN - 1] = '\0';
        }
        curr = curr->next;
    }
    
    pthread_mutex_unlock(&g_queue_mutex);
}

// Remove all requests for a specific file
void request_queue_remove_by_filename(const char *filename, const char *folder_path) {
    if (!filename || !folder_path) return;
    
    pthread_mutex_lock(&g_queue_mutex);
    
    AccessRequest *curr = g_request_queue.head;
    AccessRequest *prev = NULL;
    
    while (curr) {
        if (strcmp(curr->filename, filename) == 0 &&
            strcmp(curr->folder_path, folder_path) == 0) {
            // Remove this request
            AccessRequest *to_free = curr;
            if (prev) {
                prev->next = curr->next;
            } else {
                g_request_queue.head = curr->next;
            }
            curr = curr->next;
            free(to_free);
            g_request_queue.count--;
        } else {
            prev = curr;
            curr = curr->next;
        }
    }
    
    pthread_mutex_unlock(&g_queue_mutex);
}

// Cleanup and free all resources
void request_queue_destroy(void) {
    pthread_mutex_lock(&g_queue_mutex);
    
    AccessRequest *curr = g_request_queue.head;
    while (curr) {
        AccessRequest *next = curr->next;
        free(curr);
        curr = next;
    }
    
    g_request_queue.head = NULL;
    g_request_queue.count = 0;
    
    pthread_mutex_unlock(&g_queue_mutex);
}
