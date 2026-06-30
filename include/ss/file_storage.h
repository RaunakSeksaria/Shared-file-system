#ifndef FILE_STORAGE_H
#define FILE_STORAGE_H

#include <stddef.h>
#include <time.h>
#include "../common/acl.h"

// File storage module for Storage Server
// Handles all file operations: create, read, delete, metadata management
// Ensures data persistence across SS restarts

// Storage directory structure:
//   storage_dir/
//     ├── files/          # Actual file content
//     │   ├── file1.txt
//     │   └── file2.txt
//     └── metadata/       # File metadata (owner, timestamps, ACL, etc.)
//         ├── file1.txt.meta
//         └── file2.txt.meta

#define MAX_SENTENCE_METADATA 1024

typedef struct {
    int sentence_id;
    int version;
    size_t offset;      // byte offset within file (best effort)
    size_t length;      // byte length (best effort)
    int word_count;
    int char_count;
} SentenceMeta;

// Access request stored in metadata
#define MAX_PENDING_REQUESTS 64
typedef struct {
    int request_id;
    char requester[64];
    char access_type;  // 'R', 'W', or 'B'
    time_t timestamp;
} PendingRequest;

// File metadata structure - stores all information about a file
typedef struct {
    char owner[64];           // Username of file owner
    char folder_path[512];    // Folder path where file is located (e.g., "/" or "/folder1/")
    time_t created;           // Creation timestamp
    time_t last_modified;     // Last modification timestamp
    time_t last_accessed;     // Last access timestamp
    size_t size_bytes;        // File size in bytes
    int word_count;           // Total word count (for INFO command)
    int char_count;           // Total character count (for INFO command)
    ACL acl;                  // Access Control List (Step 4)

    // Sentence metadata (Phase 4)
    int sentence_count;
    int next_sentence_id;
    SentenceMeta sentences[MAX_SENTENCE_METADATA];
    
    // Access requests (persistent)
    int pending_request_count;
    PendingRequest pending_requests[MAX_PENDING_REQUESTS];
} FileMetadata;

// Create an empty file in the storage directory
// storage_dir: Base storage directory (e.g., "./storage_ss1")
// filename: Name of the file to create
// owner: Username of the file owner
// Returns: 0 on success, -1 on error
//
// This function:
// 1. Creates the files/ and metadata/ directories if they don't exist
// 2. Creates an empty file in files/ directory
// 3. Creates metadata file with owner and timestamps
// 4. Initializes metadata with current time
//
// Usage:
//   if (file_create("./storage_ss1", "test.txt", "alice") == 0) {
//       printf("File created successfully\n");
//   }
int file_create(const char *storage_dir, const char *filename, const char *owner);

// Read the entire content of a file
// storage_dir: Base storage directory
// filename: Name of the file to read
// content_buf: Buffer to store file content (caller allocates)
// buf_size: Size of the buffer
// actual_size: Pointer to store actual file size (can be NULL)
// Returns: 0 on success, -1 on error
//
// Note: This reads the entire file into memory. For large files,
// a streaming approach would be better, but for Phase 2 this is sufficient.
//
// Usage:
//   char content[4096];
//   size_t actual_size;
//   if (file_read("./storage_ss1", "test.txt", content, sizeof(content), &actual_size) == 0) {
//       printf("File content: %.*s\n", (int)actual_size, content);
//   }
int file_read(const char *storage_dir, const char *filename, 
              char *content_buf, size_t buf_size, size_t *actual_size);

// Read entire file content into an allocated buffer.
// Caller must free(*out_buf).
int file_read_all(const char *storage_dir, const char *filename,
                  char **out_buf, size_t *out_len);

// Write entire content to a file (for replication)
// Overwrites if file exists, creates if doesn't exist
int file_write_all(const char *storage_dir, const char *filename,
                   const char *content, size_t content_len);

// Delete a file and its metadata
// storage_dir: Base storage directory
// filename: Name of the file to delete
// Returns: 0 on success, -1 on error (file not found, permission denied, etc.)
//
// This function:
// 1. Deletes the file from files/ directory
// 2. Deletes the metadata file from metadata/ directory
// 3. Returns error if file doesn't exist
//
// Usage:
//   if (file_delete("./storage_ss1", "test.txt") == 0) {
//       printf("File deleted successfully\n");
//   }
int file_delete(const char *storage_dir, const char *filename);

// Check if a file exists in the storage directory
// storage_dir: Base storage directory
// filename: Name of the file to check
// Returns: 1 if file exists, 0 if not
//
// Usage:
//   if (file_exists("./storage_ss1", "test.txt")) {
//       printf("File exists\n");
//   }
int file_exists(const char *storage_dir, const char *filename);

// Load metadata from disk
// storage_dir: Base storage directory
// filename: Name of the file
// metadata: Pointer to FileMetadata structure to populate
// Returns: 0 on success, -1 on error (metadata file not found, parse error, etc.)
//
// This reads the metadata file (metadata/filename.meta) and parses it
// into the FileMetadata structure.
//
// Usage:
//   FileMetadata meta;
//   if (metadata_load("./storage_ss1", "test.txt", &meta) == 0) {
//       printf("Owner: %s\n", meta.owner);
//   }
int metadata_load(const char *storage_dir, const char *filename, FileMetadata *metadata);

// Save metadata to disk
// storage_dir: Base storage directory
// filename: Name of the file
// metadata: Pointer to FileMetadata structure to save
// Returns: 0 on success, -1 on error
//
// This writes the metadata to disk atomically:
// 1. Write to temporary file (metadata/filename.meta.tmp)
// 2. Rename to final file (metadata/filename.meta)
// This ensures metadata is never corrupted if SS crashes during write.
//
// Usage:
//   FileMetadata meta = {0};
//   strcpy(meta.owner, "alice");
//   meta.created = time(NULL);
//   if (metadata_save("./storage_ss1", "test.txt", &meta) == 0) {
//       printf("Metadata saved\n");
//   }
int metadata_save(const char *storage_dir, const char *filename, const FileMetadata *metadata);

// Update last accessed timestamp in metadata
// This is called whenever a file is read (for INFO command)
// storage_dir: Base storage directory
// filename: Name of the file
// Returns: 0 on success, -1 on error
//
// Usage:
//   metadata_update_last_accessed("./storage_ss1", "test.txt");
int metadata_update_last_accessed(const char *storage_dir, const char *filename);

// Update last modified timestamp in metadata
// This is called whenever a file is written (for future WRITE command)
// storage_dir: Base storage directory
// filename: Name of the file
// Returns: 0 on success, -1 on error
//
// Usage:
//   metadata_update_last_modified("./storage_ss1", "test.txt");
int metadata_update_last_modified(const char *storage_dir, const char *filename);

// Ensure sentence metadata exists (builds default sentences if missing).
int metadata_ensure_sentences(const char *storage_dir, const char *filename, FileMetadata *metadata);

// Count words and characters in file content
// This is used for INFO command to display statistics
// content: File content (null-terminated string)
// word_count: Pointer to store word count (can be NULL)
// char_count: Pointer to store character count (can be NULL)
//
// Word definition: sequence of ASCII characters without spaces
// Character count: total number of characters (including spaces)
//
// Usage:
//   int words, chars;
//   count_file_stats("Hello world!", &words, &chars);
//   // words = 2, chars = 12
void count_file_stats(const char *content, int *word_count, int *char_count);

// Ensure sentence metadata exists (build default single sentence if needed)
int metadata_ensure_sentences(const char *storage_dir, const char *filename, FileMetadata *metadata);

// Undo helpers
int undo_save_state(const char *storage_dir, const char *filename);
int undo_restore_state(const char *storage_dir, const char *filename);
int undo_exists(const char *storage_dir, const char *filename);

// ===== Checkpoint Operations =====

// Maximum checkpoints per file
#define MAX_CHECKPOINTS_PER_FILE 20

// Checkpoint metadata entry
typedef struct {
    char tag[64];           // Checkpoint tag (unique per file)
    char creator[64];       // Username who created checkpoint
    time_t timestamp;       // When checkpoint was created
    size_t file_size;       // Size of file at checkpoint
} CheckpointEntry;

// Create a checkpoint with the given tag
// storage_dir: Base storage directory
// filename: Name of the file to checkpoint
// tag: Unique tag for this checkpoint (alphanumeric, underscore, dash only)
// creator: Username creating the checkpoint
// Returns: 0 on success, -1 on error
//
// Creates:
//   storage_dir/checkpoints/filename/tag.checkpoint.data
//   storage_dir/checkpoints/filename/tag.checkpoint.meta
//   Updates checkpoint.index file
int checkpoint_create(const char *storage_dir, const char *filename, 
                     const char *tag, const char *creator);

// Check if a checkpoint exists
// Returns: 1 if exists, 0 if not
int checkpoint_exists(const char *storage_dir, const char *filename, const char *tag);

// Restore file to a checkpoint state
// This reverts both file content and metadata to the checkpoint
// Returns: 0 on success, -1 on error
int checkpoint_restore(const char *storage_dir, const char *filename, const char *tag);

// List all checkpoints for a file
// tags: Output array of checkpoint entries (caller must free)
// count: Output number of checkpoints
// Returns: 0 on success, -1 on error
int checkpoint_list(const char *storage_dir, const char *filename, 
                   CheckpointEntry **entries, int *count);

// Get content of a checkpoint (for VIEWCHECKPOINT)
// content_buf: Buffer to store checkpoint content
// buf_size: Size of buffer
// actual_size: Actual content size (can be NULL)
// Returns: 0 on success, -1 on error
int checkpoint_get_content(const char *storage_dir, const char *filename, 
                          const char *tag, char *content_buf, 
                          size_t buf_size, size_t *actual_size);

// ===== Folder Operations =====

// Create a folder (directory) on the storage server
// storage_dir: Base storage directory (e.g., "./storage_ss1")
// folder_path: Full folder path (e.g., "/folder1/subfolder/")
// Returns: 0 on success, -1 on error
//
// This function creates the directory structure in both files/ and metadata/
// Example: folder_path="/a/b/" creates:
//   storage_dir/files/a/b/
//   storage_dir/metadata/a/b/
int folder_create(const char *storage_dir, const char *folder_path);

// Move a file from one folder to another
// storage_dir: Base storage directory
// filename: Name of the file (without path)
// old_folder_path: Current folder path (e.g., "/folder1/")
// new_folder_path: Destination folder path (e.g., "/folder2/")
// Returns: 0 on success, -1 on error
//
// This moves both the file and its metadata to the new location
int file_move(const char *storage_dir, const char *filename,
              const char *old_folder_path, const char *new_folder_path);

#endif

