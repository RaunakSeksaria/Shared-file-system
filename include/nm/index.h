#ifndef INDEX_H
#define INDEX_H

#include <stddef.h>
#include <time.h>

// File Index Module for Name Server
// Provides efficient O(1) file lookup using hash map
// Also includes LRU cache for frequently accessed files

// Maximum filename length
#define MAX_FILENAME 256
#define MAX_FOLDER_PATH 512
#define MAX_SS_HOST 64
#define MAX_SS_USERNAME 64

// Structure representing a file entry in the index
// This stores all metadata needed for file operations and VIEW/INFO commands
typedef struct FileEntry {
    char filename[MAX_FILENAME];      // Name of the file (without path)
    char folder_path[MAX_FOLDER_PATH]; // Folder path (e.g., "/" or "/folder1/folder2/")
    char owner[64];                   // Username of file owner
    char ss_host[64];                 // IP address of Storage Server hosting this file
    int ss_client_port;               // Port on SS for client connections
    char ss_username[64];             // Username of SS (for identification)
    
    // Replication fields (for fault tolerance)
    char replica_ss_host[64];         // Backup SS IP address
    int replica_ss_client_port;       // Backup SS client port
    char replica_ss_username[64];     // Backup SS username
    
    time_t created;                   // Creation timestamp
    time_t last_modified;             // Last modification timestamp
    time_t last_accessed;             // Last access timestamp
    size_t size_bytes;                // File size in bytes
    int word_count;                   // Word count (for INFO command)
    int char_count;                   // Character count (for INFO command)
    
    // Internal: for hash map chaining
    struct FileEntry *next;
    
    // Internal: for LRU cache (doubly-linked list)
    struct FileEntry *lru_prev;
    struct FileEntry *lru_next;
} FileEntry;

// Hash map structure for O(1) file lookup
// Uses chaining to handle collisions
#define INDEX_HASH_SIZE 1024  // Hash table size (power of 2 for efficiency)

// Structure representing a folder in the index
// Tracks folder hierarchy for CREATEFOLDER and VIEWFOLDER operations
typedef struct FolderEntry {
    char folder_path[MAX_FOLDER_PATH]; // Full path (e.g., "/folder1/folder2/")
    time_t created;                     // Creation timestamp
    char ss_username[64];               // SS where folder exists
    
    // Internal: for hash map chaining
    struct FolderEntry *next;
} FolderEntry;

// Folder hash map structure (use same hash size as files)
typedef struct {
    FolderEntry *buckets[INDEX_HASH_SIZE];
    int count;
} FolderIndex;

// Global folder index
extern FolderIndex g_folder_index;

typedef struct {
    FileEntry *buckets[INDEX_HASH_SIZE];  // Hash buckets (array of linked lists)
    int count;                             // Total number of files indexed
} FileIndex;

// LRU Cache structure for frequently accessed files
// Recent lookups are cached for faster subsequent access
#define LRU_CACHE_SIZE 100  // Maximum files in cache

typedef struct {
    FileEntry *head;  // Most recently used
    FileEntry *tail;  // Least recently used
    int size;         // Current cache size
} LRUCache;

// Global index (one per NM instance)
// Initialized once and used throughout NM lifetime
extern FileIndex g_file_index;
extern LRUCache g_lru_cache;

// Initialize the file index and LRU cache
// Must be called before any other index operations
void index_init(void);

// Add a file to the index
// Called when SS registers with file list, or when file is created
// filename: Name of the file
// owner: Username of file owner
// ss_host: IP address of Storage Server
// ss_client_port: Port on SS for client connections
// ss_username: Username of SS
// Returns: Pointer to created FileEntry, or NULL on error
//
// This function:
// 1. Hashes the filename to find the bucket
// 2. Checks if file already exists (returns existing entry if found)
// 3. Creates new FileEntry and adds to hash map
// 4. Increments file count
//
// Usage:
//   FileEntry *entry = index_add_file("test.txt", "alice", "127.0.0.1", 6001, "ss1");
FileEntry *index_add_file(const char *filename, const char *owner,
                          const char *ss_host, int ss_client_port,
                          const char *ss_username);

// Remove a file from the index
// Called when file is deleted
// filename: Name of the file to remove
// Returns: 0 on success, -1 if file not found
//
// This function:
// 1. Hashes filename to find bucket
// 2. Searches chain for file
// 3. Removes from hash map
// 4. Removes from LRU cache if present
// 5. Decrements file count
int index_remove_file(const char *filename);

// Lookup a file in the index (O(1) average case)
// filename: Name of the file to find
// Returns: Pointer to FileEntry if found, NULL otherwise
//
// This function:
// 1. Hashes filename to find bucket
// 2. Searches chain for matching file
// 3. Updates LRU cache (moves to front if found in cache)
// 4. Returns entry or NULL
//
// Usage:
//   FileEntry *entry = index_lookup_file("test.txt");
//   if (entry) {
//       printf("File found: owner=%s\n", entry->owner);
//   }
FileEntry *index_lookup_file(const char *filename);

// Get all files in the index
// files: Array to populate with FileEntry pointers
// max_files: Maximum number of files to return
// Returns: Number of files actually returned
//
// This iterates through all hash buckets and collects all files
// Used for VIEW command (lists all files)
int index_get_all_files(FileEntry **files, int max_files);

// Get files owned by a specific user
// owner: Username of file owner
// files: Array to populate with FileEntry pointers
// max_files: Maximum number of files to return
// Returns: Number of files found
//
// Iterates through all files and filters by owner
// Used for VIEW command (lists user's files)
int index_get_files_by_owner(const char *owner, FileEntry **files, int max_files);

// Update file metadata in index
// filename: Name of the file
// Updates: last_accessed, last_modified, size_bytes, word_count, char_count
// Returns: 0 on success, -1 if file not found
//
// This is called when file is accessed (READ) or modified (WRITE)
// Updates the in-memory index (metadata file on SS is updated separately)
int index_update_metadata(const char *filename, time_t last_accessed,
                          time_t last_modified, size_t size_bytes,
                          int word_count, int char_count);

// Hash function for filename (simple djb2 hash)
// Returns: Hash value (0 to INDEX_HASH_SIZE-1)
unsigned int index_hash(const char *filename);

// ===== Folder Management Functions =====

// Add a folder to the index
// folder_path: Full folder path (e.g., "/folder1/subfolder2/")
// ss_username: Username of SS where folder exists
// Returns: Pointer to created FolderEntry, or NULL on error
FolderEntry *index_add_folder(const char *folder_path, const char *ss_username);

// Check if a folder exists
// folder_path: Full folder path to check
// Returns: 1 if exists, 0 if not
int index_folder_exists(const char *folder_path);

// Get all files in a specific folder (not recursive)
// folder_path: Folder path to list (e.g., "/folder1/")
// files: Array to populate with FileEntry pointers
// max_files: Maximum number of files to return
// Returns: Number of files found in the folder
int index_get_files_in_folder(const char *folder_path, FileEntry **files, int max_files);

// Get all subfolders in a specific folder (not recursive)
// folder_path: Parent folder path (e.g., "/folder1/")
// folders: Array to populate with FolderEntry pointers
// max_folders: Maximum number of folders to return
// Returns: Number of subfolders found
int index_get_subfolders(const char *folder_path, FolderEntry **folders, int max_folders);

// Update file's folder path (for MOVE operation)
// filename: Name of the file
// old_folder_path: Current folder path
// new_folder_path: New folder path
// Returns: 0 on success, -1 on error
int index_move_file(const char *filename, const char *old_folder_path, 
                    const char *new_folder_path);

#endif

