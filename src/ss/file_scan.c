#include "file_scan.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "file_storage.h"

// Helper: Recursively scan a directory and its subdirectories
static void scan_directory_recursive(const char *storage_dir, const char *rel_path, 
                                    const char *files_base, ScanResult *result) {
    if (!storage_dir || !rel_path || !files_base || !result) return;
    if (result->count >= MAX_FILES_PER_SS) return;
    
    // Build full path to current directory
    char dir_path[768];
    snprintf(dir_path, sizeof(dir_path), "%s/%s", files_base, rel_path);
    
    DIR *dir = opendir(dir_path);
    if (!dir) return;
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && result->count < MAX_FILES_PER_SS) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        // Build full path to this entry
        char entry_path[1024];
        snprintf(entry_path, sizeof(entry_path), "%s/%s", dir_path, entry->d_name);
        
        struct stat st;
        if (stat(entry_path, &st) != 0) {
            continue;
        }
        
        // If it's a directory, recurse into it
        if (S_ISDIR(st.st_mode)) {
            char new_rel_path[768];
            if (strcmp(rel_path, "") == 0) {
                snprintf(new_rel_path, sizeof(new_rel_path), "%s", entry->d_name);
            } else {
                snprintf(new_rel_path, sizeof(new_rel_path), "%s/%s", rel_path, entry->d_name);
            }
            scan_directory_recursive(storage_dir, new_rel_path, files_base, result);
        }
        // If it's a regular file, add it to results
        else if (S_ISREG(st.st_mode)) {
            ScannedFile *file = &result->files[result->count];
            
            // Store relative path including folder structure
            if (strcmp(rel_path, "") == 0) {
                size_t len = strlen(entry->d_name);
                if (len >= sizeof(file->filename)) len = sizeof(file->filename) - 1;
                memcpy(file->filename, entry->d_name, len);
                file->filename[len] = '\0';
            } else {
                int n = snprintf(file->filename, sizeof(file->filename), "/%s/%s", rel_path, entry->d_name);
                if (n < 0 || (size_t)n >= sizeof(file->filename)) {
                    // Path too long, skip
                    continue;
                }
            }
            file->size_bytes = (size_t)st.st_size;
            
            // Check if metadata file exists
            // Normalize filename (strip leading slash) for metadata path
            const char *norm_filename = file->filename;
            if (norm_filename[0] == '/') norm_filename++;
            char meta_path[1024];
            snprintf(meta_path, sizeof(meta_path), "%s/metadata/%s.meta", storage_dir, norm_filename);
            file->has_metadata = (access(meta_path, F_OK) == 0) ? 1 : 0;
            
            result->count++;
        }
    }
    
    closedir(dir);
}

// Scan the storage directory for existing files
// This is called during SS startup to discover what files already exist
ScanResult scan_directory(const char *storage_dir, const char *files_dir) {
    ScanResult result = {0};  // Initialize with count = 0
    
    // Build full path to files directory: storage_dir/files_dir
    // Example: "./storage_ss1/files"
    char files_path[512];
    snprintf(files_path, sizeof(files_path), "%s/%s", storage_dir, files_dir);
    
    // Check if directory exists
    DIR *dir = opendir(files_path);
    if (!dir) {
        // Directory doesn't exist yet - that's OK, just return empty result
        return result;
    }
    closedir(dir);
    
    // Recursively scan the directory and all subdirectories
    scan_directory_recursive(storage_dir, "", files_path, &result);
    
    return result;
}

// Build a comma-separated file list string from scan results
// This string is sent to NM in the SS_REGISTER payload
int build_file_list_string(const ScanResult *result, const char *storage_dir,
                           char *buf, size_t buflen) {
    if (!result || !buf || buflen == 0 || !storage_dir) return -1;
    
    // Start with empty string
    buf[0] = '\0';
    size_t pos = 0;
    
    // If no files, return empty string (that's OK)
    if (result->count == 0) {
        return 0;
    }
    
    // Build comma-separated list: "file1.txt,file2.txt,file3.txt"
    for (int i = 0; i < result->count; i++) {
        const char *filename = result->files[i].filename;

        FileMetadata meta = {0};
        char owner[64] = {0};
        size_t size_bytes = 0;
        int words = 0;
        int chars = 0;
        if (metadata_load(storage_dir, filename, &meta) == 0) {
            snprintf(owner, sizeof(owner), "%s", meta.owner);
            size_bytes = meta.size_bytes;
            words = meta.word_count;
            chars = meta.char_count;
        }

        char entry[512];
        int entry_len = snprintf(entry, sizeof(entry), "%s|%s|%zu|%d|%d",
                                 filename,
                                 owner,
                                 size_bytes,
                                 words,
                                 chars);
        if (entry_len < 0) {
            return -1;
        }

        size_t needed = (size_t)entry_len + ((i < result->count - 1) ? 1 : 0);
        if (pos + needed + 1 >= buflen) {
            return -1;
        }

        memcpy(buf + pos, entry, (size_t)entry_len);
        pos += (size_t)entry_len;

        if (i < result->count - 1) {
            buf[pos++] = ',';
        }
    }
    
    buf[pos] = '\0';  // Ensure null termination
    return 0;
}

