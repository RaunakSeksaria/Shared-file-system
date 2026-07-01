#define _GNU_SOURCE  /* for PTHREAD_MUTEX_RECURSIVE */
#include "common/xalloc.h"
#include "nm/index.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

// Global index and cache instances
FileIndex g_file_index = {0};
LRUCache g_lru_cache = {0};
FolderIndex g_folder_index = {0};

/* Thread-per-connection NM: this recursive mutex serializes all access to the
   shared file/folder index + LRU cache. Recursive because some entry points
   (index_add_file/move_file/update_metadata) call index_lookup_file while
   already holding it. Initialized in index_init(). */
static pthread_mutex_t g_index_mu;

// Initialize the file index and LRU cache
// Sets up empty hash table and empty cache
void index_init(void) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_index_mu, &attr);
    pthread_mutexattr_destroy(&attr);

    // Clear hash table (all buckets are NULL)
    memset(g_file_index.buckets, 0, sizeof(g_file_index.buckets));
    g_file_index.count = 0;
    
    // Clear LRU cache
    g_lru_cache.head = NULL;
    g_lru_cache.tail = NULL;
    g_lru_cache.size = 0;
    
    // Clear folder index
    memset(g_folder_index.buckets, 0, sizeof(g_folder_index.buckets));
    g_folder_index.count = 0;
    
    // Add root folder by default
    index_add_folder("/", "");
}

// Hash function: djb2 hash algorithm (simple and effective)
// Converts filename string to hash value for bucket selection
unsigned int index_hash(const char *filename) {
    unsigned long hash = 5381;
    int c;
    
    // Hash each character: hash = hash * 33 + c
    while ((c = *filename++)) {
        hash = ((hash << 5) + hash) + c;  // hash * 33 + c
    }
    
    // Modulo to get bucket index (0 to INDEX_HASH_SIZE-1)
    return hash % INDEX_HASH_SIZE;
}

// Add entry to LRU cache (at front - most recently used)
// This is called when a file is accessed (looked up)
static void lru_add_to_front(FileEntry *entry) {
    if (!entry) return;
    
    // Remove from current position if already in cache
    if (entry->lru_prev) {
        entry->lru_prev->lru_next = entry->lru_next;
    } else if (g_lru_cache.head == entry) {
        g_lru_cache.head = entry->lru_next;
    }
    
    if (entry->lru_next) {
        entry->lru_next->lru_prev = entry->lru_prev;
    } else if (g_lru_cache.tail == entry) {
        g_lru_cache.tail = entry->lru_prev;
    }
    
    // Add to front
    entry->lru_prev = NULL;
    entry->lru_next = g_lru_cache.head;
    
    if (g_lru_cache.head) {
        g_lru_cache.head->lru_prev = entry;
    }
    g_lru_cache.head = entry;
    
    if (!g_lru_cache.tail) {
        g_lru_cache.tail = entry;
    }
    
    g_lru_cache.size++;
}

// Remove least recently used entry from cache (when cache is full)
static void lru_remove_tail(void) {
    if (!g_lru_cache.tail) return;
    
    FileEntry *old_tail = g_lru_cache.tail;
    g_lru_cache.tail = old_tail->lru_prev;
    
    if (g_lru_cache.tail) {
        g_lru_cache.tail->lru_next = NULL;
    } else {
        g_lru_cache.head = NULL;
    }
    
    old_tail->lru_prev = NULL;
    old_tail->lru_next = NULL;
    g_lru_cache.size--;
}

// Add a file to the index
FileEntry *index_add_file(const char *filename, const char *owner,
                          const char *ss_host, int ss_client_port,
                          const char *ss_username) {
    pthread_mutex_lock(&g_index_mu);
    if (!filename) { pthread_mutex_unlock(&g_index_mu); return NULL; }
    
    // Check if file already exists
    FileEntry *existing = index_lookup_file(filename);
    if (existing) {
        // Update SS information (in case SS re-registered)
        if (ss_host) strncpy(existing->ss_host, ss_host, sizeof(existing->ss_host) - 1);
        existing->ss_client_port = ss_client_port;
        if (ss_username) strncpy(existing->ss_username, ss_username, sizeof(existing->ss_username) - 1);
        pthread_mutex_unlock(&g_index_mu);
        return existing;
    }
    
    // Create new file entry
    FileEntry *entry = (FileEntry *)xcalloc(1, sizeof(FileEntry));
    if (!entry) { pthread_mutex_unlock(&g_index_mu); return NULL; }
    
    // Parse filename to extract folder path and base filename
    // Format can be "/folder/file.txt" or just "file.txt" (root folder)
    const char *last_slash = strrchr(filename, '/');
    if (last_slash) {
        // File has a folder path
        size_t folder_len = last_slash - filename + 1; // Include the trailing /
        if (folder_len < sizeof(entry->folder_path)) {
            memcpy(entry->folder_path, filename, folder_len);
            entry->folder_path[folder_len] = '\0';
        } else {
            // Path too long, default to root
            snprintf(entry->folder_path, sizeof(entry->folder_path), "/");
        }
        // Copy just the filename part (after last /)
        strncpy(entry->filename, last_slash + 1, sizeof(entry->filename) - 1);
    } else {
        // No path separator, file is in root folder
        snprintf(entry->folder_path, sizeof(entry->folder_path), "/");
        strncpy(entry->filename, filename, sizeof(entry->filename) - 1);
    }
    
    // Copy owner (leave empty if NULL - will be loaded from metadata later)
    if (owner) {
        strncpy(entry->owner, owner, sizeof(entry->owner) - 1);
    } else {
        entry->owner[0] = '\0';  // Empty string indicates owner not yet loaded
    }
    
    // Copy SS information
    if (ss_host) {
        strncpy(entry->ss_host, ss_host, sizeof(entry->ss_host) - 1);
    }
    entry->ss_client_port = ss_client_port;
    if (ss_username) {
        strncpy(entry->ss_username, ss_username, sizeof(entry->ss_username) - 1);
    }
    
    // Initialize timestamps
    time_t now = time(NULL);
    entry->created = now;
    entry->last_modified = now;
    entry->last_accessed = now;
    
    // Initialize counts
    entry->size_bytes = 0;
    entry->word_count = 0;
    entry->char_count = 0;
    
    // Add to hash map
    // Hash the base filename (not the full path) for consistent lookups
    unsigned int hash = index_hash(entry->filename);
    entry->next = g_file_index.buckets[hash];
    g_file_index.buckets[hash] = entry;
    g_file_index.count++;
    
    pthread_mutex_unlock(&g_index_mu);
    
    return entry;
}

// Remove a file from the index
int index_remove_file(const char *filename) {
    pthread_mutex_lock(&g_index_mu);
    if (!filename) { pthread_mutex_unlock(&g_index_mu); return -1; }
    
    // Parse input to get folder path and base filename
    char folder_path[MAX_FOLDER_PATH] = "/";
    char base_filename[MAX_FILENAME];
    
    const char *last_slash = strrchr(filename, '/');
    if (last_slash) {
        size_t folder_len = last_slash - filename + 1;
        if (folder_len < sizeof(folder_path)) {
            memcpy(folder_path, filename, folder_len);
            folder_path[folder_len] = '\0';
        }
        strncpy(base_filename, last_slash + 1, sizeof(base_filename) - 1);
        base_filename[sizeof(base_filename) - 1] = '\0';
    } else {
        strncpy(base_filename, filename, sizeof(base_filename) - 1);
        base_filename[sizeof(base_filename) - 1] = '\0';
    }
    
    unsigned int hash = index_hash(base_filename);
    FileEntry *curr = g_file_index.buckets[hash];
    FileEntry *prev = NULL;
    
    // Search for file in hash bucket chain
    while (curr) {
        if (strcmp(curr->filename, base_filename) == 0 &&
            strcmp(curr->folder_path, folder_path) == 0) {
            // Found it - remove from chain
            if (prev) {
                prev->next = curr->next;
            } else {
                g_file_index.buckets[hash] = curr->next;
            }
            
            // Remove from LRU cache if present
            if (curr->lru_prev || curr->lru_next || g_lru_cache.head == curr) {
                if (curr->lru_prev) {
                    curr->lru_prev->lru_next = curr->lru_next;
                } else {
                    g_lru_cache.head = curr->lru_next;
                }
                if (curr->lru_next) {
                    curr->lru_next->lru_prev = curr->lru_prev;
                } else {
                    g_lru_cache.tail = curr->lru_prev;
                }
                if (g_lru_cache.head == curr) g_lru_cache.head = NULL;
                if (g_lru_cache.tail == curr) g_lru_cache.tail = NULL;
                g_lru_cache.size--;
            }
            
            free(curr);
            g_file_index.count--;
            pthread_mutex_unlock(&g_index_mu);
            return 0;
        }
        prev = curr;
        curr = curr->next;
    }
    
    pthread_mutex_unlock(&g_index_mu);
    
    return -1;  // Not found
}

// Lookup a file in the index (O(1) average case)
FileEntry *index_lookup_file(const char *filename) {
    pthread_mutex_lock(&g_index_mu);
    if (!filename) { pthread_mutex_unlock(&g_index_mu); return NULL; }
    
    // Parse input to get folder path and base filename
    char folder_path[MAX_FOLDER_PATH];
    char base_filename[MAX_FILENAME];
    
    const char *last_slash = strrchr(filename, '/');
    if (last_slash) {
        // Input has folder path with trailing /
        size_t folder_len = last_slash - filename + 1;
        if (folder_len < sizeof(folder_path)) {
            memcpy(folder_path, filename, folder_len);
            folder_path[folder_len] = '\0';
        } else {
            snprintf(folder_path, sizeof(folder_path), "/");
        }
        strncpy(base_filename, last_slash + 1, sizeof(base_filename) - 1);
        base_filename[sizeof(base_filename) - 1] = '\0';
    } else {
        // No folder path, use root folder
        snprintf(folder_path, sizeof(folder_path), "/");
        strncpy(base_filename, filename, sizeof(base_filename) - 1);
        base_filename[sizeof(base_filename) - 1] = '\0';
    }
    
    unsigned int hash = index_hash(base_filename);
    FileEntry *curr = g_file_index.buckets[hash];
    
    // Search chain for matching filename AND folder_path
    while (curr) {
        if (strcmp(curr->filename, base_filename) == 0 &&
            strcmp(curr->folder_path, folder_path) == 0) {
            // Found - update LRU cache
            // If cache is full, remove least recently used
            if (g_lru_cache.size >= LRU_CACHE_SIZE && 
                (!curr->lru_prev && !curr->lru_next && g_lru_cache.head != curr)) {
                lru_remove_tail();
            }
            lru_add_to_front(curr);
            pthread_mutex_unlock(&g_index_mu);
            return curr;
        }
        curr = curr->next;
    }
    
    pthread_mutex_unlock(&g_index_mu);
    
    return NULL;  // Not found
}

// Get all files in the index
int index_get_all_files(FileEntry **files, int max_files) {
    pthread_mutex_lock(&g_index_mu);
    if (!files || max_files <= 0) { pthread_mutex_unlock(&g_index_mu); return 0; }
    
    int count = 0;
    
    // Iterate through all hash buckets
    for (int i = 0; i < INDEX_HASH_SIZE && count < max_files; i++) {
        FileEntry *curr = g_file_index.buckets[i];
        while (curr && count < max_files) {
            files[count++] = curr;
            curr = curr->next;
        }
    }
    
    pthread_mutex_unlock(&g_index_mu);
    
    return count;
}

// Get files owned by a specific user
int index_get_files_by_owner(const char *owner, FileEntry **files, int max_files) {
    pthread_mutex_lock(&g_index_mu);
    if (!owner || !files || max_files <= 0) { pthread_mutex_unlock(&g_index_mu); return 0; }
    
    int count = 0;
    
    // Iterate through all hash buckets
    for (int i = 0; i < INDEX_HASH_SIZE && count < max_files; i++) {
        FileEntry *curr = g_file_index.buckets[i];
        while (curr && count < max_files) {
            // Filter by owner
            if (strcmp(curr->owner, owner) == 0) {
                files[count++] = curr;
            }
            curr = curr->next;
        }
    }
    
    pthread_mutex_unlock(&g_index_mu);
    
    return count;
}

// Update file metadata in index
int index_update_metadata(const char *filename, time_t last_accessed,
                          time_t last_modified, size_t size_bytes,
                          int word_count, int char_count) {
    pthread_mutex_lock(&g_index_mu);
    FileEntry *entry = index_lookup_file(filename);
    if (!entry) { pthread_mutex_unlock(&g_index_mu); return -1; }
    
    if (last_accessed > 0) entry->last_accessed = last_accessed;
    if (last_modified > 0) entry->last_modified = last_modified;
    entry->size_bytes = size_bytes;
    entry->word_count = word_count;
    entry->char_count = char_count;
    
    pthread_mutex_unlock(&g_index_mu);
    
    return 0;
}

// ===== Folder Management Functions =====

// Add a folder to the index
// Normalize a folder path to canonical trailing-slash form: "/" stays "/", and
// "/docs" becomes "/docs/". Folders are stored, looked up, and matched in this
// form, so every folder operation must normalize through this one helper.
static void normalize_folder_path(const char *in, char *out, size_t outlen) {
    size_t len = strlen(in);
    if (strcmp(in, "/") == 0) {
        snprintf(out, outlen, "/");
    } else if (len > 0 && in[len - 1] == '/') {
        snprintf(out, outlen, "%s", in);
    } else {
        snprintf(out, outlen, "%s/", in);
    }
}

FolderEntry *index_add_folder(const char *folder_path, const char *ss_username) {
    pthread_mutex_lock(&g_index_mu);
    if (!folder_path) { pthread_mutex_unlock(&g_index_mu); return NULL; }

    char normalized_path[MAX_FOLDER_PATH];
    normalize_folder_path(folder_path, normalized_path, sizeof(normalized_path));
    
    // Check if folder already exists
    unsigned int hash = index_hash(normalized_path);
    FolderEntry *curr = g_folder_index.buckets[hash];
    while (curr) {
        if (strcmp(curr->folder_path, normalized_path) == 0) {
            // Already exists, update SS username if provided
            if (ss_username && ss_username[0]) {
                strncpy(curr->ss_username, ss_username, sizeof(curr->ss_username) - 1);
            }
            pthread_mutex_unlock(&g_index_mu);
            return curr;
        }
        curr = curr->next;
    }
    
    // Create new folder entry
    FolderEntry *entry = (FolderEntry *)xcalloc(1, sizeof(FolderEntry));
    if (!entry) { pthread_mutex_unlock(&g_index_mu); return NULL; }
    
    size_t copy_len = strlen(normalized_path);
    if (copy_len >= sizeof(entry->folder_path)) {
        copy_len = sizeof(entry->folder_path) - 1;
    }
    memcpy(entry->folder_path, normalized_path, copy_len);
    entry->folder_path[copy_len] = '\0';
    entry->created = time(NULL);
    if (ss_username && ss_username[0]) {
        strncpy(entry->ss_username, ss_username, sizeof(entry->ss_username) - 1);
    }
    
    // Add to hash map
    entry->next = g_folder_index.buckets[hash];
    g_folder_index.buckets[hash] = entry;
    g_folder_index.count++;
    
    pthread_mutex_unlock(&g_index_mu);
    
    return entry;
}

// Check if a folder exists in the index
int index_folder_exists(const char *folder_path) {
    pthread_mutex_lock(&g_index_mu);
    if (!folder_path) { pthread_mutex_unlock(&g_index_mu); return 0; }

    char normalized_path[MAX_FOLDER_PATH];
    normalize_folder_path(folder_path, normalized_path, sizeof(normalized_path));
    
    unsigned int hash = index_hash(normalized_path);
    FolderEntry *curr = g_folder_index.buckets[hash];
    
    while (curr) {
        if (strcmp(curr->folder_path, normalized_path) == 0) {
            pthread_mutex_unlock(&g_index_mu);
            return 1;
        }
        curr = curr->next;
    }
    
    pthread_mutex_unlock(&g_index_mu);
    
    return 0;
}

// Get all files in a specific folder (not recursive)
int index_get_files_in_folder(const char *folder_path, FileEntry **files, int max_files) {
    pthread_mutex_lock(&g_index_mu);
    if (!folder_path || !files || max_files <= 0) { pthread_mutex_unlock(&g_index_mu); return 0; }

    char normalized_path[MAX_FOLDER_PATH];
    normalize_folder_path(folder_path, normalized_path, sizeof(normalized_path));
    
    int count = 0;
    
    // Iterate through all hash buckets
    for (int i = 0; i < INDEX_HASH_SIZE && count < max_files; i++) {
        FileEntry *curr = g_file_index.buckets[i];
        while (curr && count < max_files) {
            // Check if file is in the specified folder
            if (strcmp(curr->folder_path, normalized_path) == 0) {
                files[count++] = curr;
            }
            curr = curr->next;
        }
    }
    
    pthread_mutex_unlock(&g_index_mu);
    
    return count;
}

// Get all subfolders in a specific folder (not recursive)
int index_get_subfolders(const char *folder_path, FolderEntry **folders, int max_folders) {
    pthread_mutex_lock(&g_index_mu);
    if (!folder_path || !folders || max_folders <= 0) { pthread_mutex_unlock(&g_index_mu); return 0; }
    
    int count = 0;
    size_t parent_len = strlen(folder_path);
    
    // Ensure parent path ends with /
    int parent_has_slash = (parent_len > 0 && folder_path[parent_len - 1] == '/');
    
    // Iterate through all folder buckets
    for (int i = 0; i < INDEX_HASH_SIZE && count < max_folders; i++) {
        FolderEntry *curr = g_folder_index.buckets[i];
        while (curr && count < max_folders) {
            // Check if this folder is a direct child of the parent folder
            // Parent: "/a/" -> child: "/a/b/" (not "/a/b/c/")
            if (strncmp(curr->folder_path, folder_path, parent_len) == 0) {
                const char *rest = curr->folder_path + parent_len;
                if (!parent_has_slash && rest[0] == '/') rest++;
                
                // Check if rest contains exactly one more folder level
                // Should have exactly one '/' at the end
                const char *slash = strchr(rest, '/');
                if (slash && slash[1] == '\0') {
                    // This is a direct child
                    folders[count++] = curr;
                }
            }
            curr = curr->next;
        }
    }
    
    pthread_mutex_unlock(&g_index_mu);
    
    return count;
}

// Update file's folder path (for MOVE operation)
int index_move_file(const char *filename, const char *old_folder_path, 
                    const char *new_folder_path) {
    pthread_mutex_lock(&g_index_mu);
    if (!filename || !old_folder_path || !new_folder_path) { pthread_mutex_unlock(&g_index_mu); return -1; }
    
    // Build full old path
    char old_full_path[MAX_FOLDER_PATH + MAX_FILENAME];
    snprintf(old_full_path, sizeof(old_full_path), "%s%s", old_folder_path, filename);
    
    // Look up the file
    FileEntry *entry = index_lookup_file(old_full_path);
    if (!entry) { pthread_mutex_unlock(&g_index_mu); return -1; }

    // Store the destination in canonical trailing-slash form so VIEWFOLDER's
    // normalized lookup matches it (the bug stored "/docs", not "/docs/").
    char normalized_path[MAX_FOLDER_PATH];
    normalize_folder_path(new_folder_path, normalized_path, sizeof(normalized_path));
    strncpy(entry->folder_path, normalized_path, sizeof(entry->folder_path) - 1);
    entry->folder_path[sizeof(entry->folder_path) - 1] = '\0';

    pthread_mutex_unlock(&g_index_mu);

    return 0;
}

