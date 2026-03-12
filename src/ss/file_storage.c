#include "file_storage.h"
#include "sentence_parser.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int copy_file_atomic(const char *src, const char *dst) {
    if (!src || !dst) return -1;
    FILE *in = fopen(src, "rb");
    if (!in) return -1;
    char tmp_path[520];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", dst);
    FILE *out = fopen(tmp_path, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }
    char buffer[4096];
    size_t n;
    int result = 0;
    while ((n = fread(buffer, 1, sizeof(buffer), in)) > 0) {
        if (fwrite(buffer, 1, n, out) != n) {
            result = -1;
            break;
        }
    }
    if (ferror(in)) result = -1;
    fflush(out);
    fsync(fileno(out));
    fclose(in);
    fclose(out);
    if (result == 0) {
        if (rename(tmp_path, dst) != 0) {
            unlink(tmp_path);
            result = -1;
        }
    } else {
        unlink(tmp_path);
    }
    return result;
}

// Helper: Normalize filename by removing leading slash
// Files are stored without leading slash (e.g., "documents/a.txt" not "/documents/a.txt")
static const char *normalize_filename(const char *filename) {
    if (!filename) return filename;
    // Skip leading slash if present
    if (filename[0] == '/') {
        return filename + 1;
    }
    return filename;
}

static void build_undo_paths(const char *storage_dir, const char *filename,
                             char *meta_path, size_t meta_len,
                             char *data_path, size_t data_len) {
    const char *norm_filename = normalize_filename(filename);
    snprintf(meta_path, meta_len, "%s/metadata/%s.undo.meta", storage_dir, norm_filename);
    snprintf(data_path, data_len, "%s/metadata/%s.undo.data", storage_dir, norm_filename);
}

// Helper: Ensure directories exist (create if they don't)
static void ensure_directories(const char *storage_dir) {
    char files_dir[512];
    char meta_dir[512];
    
    snprintf(files_dir, sizeof(files_dir), "%s/files", storage_dir);
    snprintf(meta_dir, sizeof(meta_dir), "%s/metadata", storage_dir);
    
    // Create directories (mkdir -p equivalent)
    // mkdir() returns 0 on success, -1 if directory exists or error
    mkdir(storage_dir, 0755);
    mkdir(files_dir, 0755);
    mkdir(meta_dir, 0755);
}

// Create an empty file with metadata
int file_create(const char *storage_dir, const char *filename, const char *owner) {
    if (!storage_dir || !filename || !owner) return -1;
    
    // Ensure directories exist
    ensure_directories(storage_dir);
    
    // Parse filename to extract folder path
    char folder_path[512] = "/";
    const char *last_slash = strrchr(filename, '/');
    if (last_slash) {
        size_t folder_len = last_slash - filename + 1;
        if (folder_len < sizeof(folder_path)) {
            memcpy(folder_path, filename, folder_len);
            folder_path[folder_len] = '\0';
        }
        
        // Create folder structure if it doesn't exist
        if (strcmp(folder_path, "/") != 0) {
            folder_create(storage_dir, folder_path);
        }
    }
    
    // Build paths (filename may include folder path)
    // Normalize filename (remove leading slash if present)
    const char *norm_filename = normalize_filename(filename);
    char file_path[512];
    snprintf(file_path, sizeof(file_path), "%s/files/%s", storage_dir, norm_filename);
    
    // Check if file already exists
    if (access(file_path, F_OK) == 0) {
        // File exists - return error (will be handled by caller)
        return -1;
    }
    
    // Create empty file
    FILE *fp = fopen(file_path, "w");
    if (!fp) {
        return -1;  // Failed to create file
    }
    fclose(fp);
    
    // Create metadata
    FileMetadata meta = {0};
    size_t owner_len = strlen(owner);
    if (owner_len >= sizeof(meta.owner)) owner_len = sizeof(meta.owner) - 1;
    memcpy(meta.owner, owner, owner_len);
    meta.owner[owner_len] = '\0';
    
    size_t folder_len = strlen(folder_path);
    if (folder_len >= sizeof(meta.folder_path)) folder_len = sizeof(meta.folder_path) - 1;
    memcpy(meta.folder_path, folder_path, folder_len);
    meta.folder_path[folder_len] = '\0';
    
    time_t now = time(NULL);
    meta.created = now;
    meta.last_modified = now;
    meta.last_accessed = now;
    meta.size_bytes = 0;
    meta.word_count = 0;
    meta.char_count = 0;
    meta.sentence_count = 0;
    meta.next_sentence_id = 1;
    
    // Initialize ACL with owner (Step 4)
    meta.acl = acl_init(owner);
    
    // Save metadata
    if (metadata_save(storage_dir, filename, &meta) != 0) {
        // Failed to save metadata - clean up file
        unlink(file_path);
        return -1;
    }
    
    return 0;  // Success
}

// Read entire file content
int file_read(const char *storage_dir, const char *filename,
              char *content_buf, size_t buf_size, size_t *actual_size) {
    if (!storage_dir || !filename || !content_buf || buf_size == 0) return -1;
    
    // Build file path
    const char *norm_filename = normalize_filename(filename);
    char file_path[512];
    snprintf(file_path, sizeof(file_path), "%s/files/%s", storage_dir, norm_filename);
    snprintf(file_path, sizeof(file_path), "%s/files/%s", storage_dir, norm_filename);
    
    // Open file for reading
    FILE *fp = fopen(file_path, "r");
    if (!fp) {
        return -1;  // File not found or can't open
    }
    
    // Read file content
    size_t read_bytes = fread(content_buf, 1, buf_size - 1, fp);
    fclose(fp);
    
    // Ensure null termination
    content_buf[read_bytes] = '\0';
    
    // Set actual size if pointer provided
    if (actual_size) {
        *actual_size = read_bytes;
    }
    
    return 0;  // Success
}

int file_read_all(const char *storage_dir, const char *filename,
                  char **out_buf, size_t *out_len) {
    if (!storage_dir || !filename || !out_buf) return -1;

    char file_path[512];
    snprintf(file_path, sizeof(file_path), "%s/files/%s", storage_dir, filename);

    FILE *fp = fopen(file_path, "r");
    if (!fp) {
        return -1;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }
    long size = ftell(fp);
    if (size < 0) {
        fclose(fp);
        return -1;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return -1;
    }
    char *buffer = (char *)malloc((size_t)size + 1);
    if (!buffer) {
        fclose(fp);
        return -1;
    }
    size_t read_bytes = fread(buffer, 1, (size_t)size, fp);
    fclose(fp);
    buffer[read_bytes] = '\0';
    *out_buf = buffer;
    if (out_len) {
        *out_len = read_bytes;
    }
    return 0;
}

// Write entire content to a file
// Overwrites if file exists, creates if doesn't exist
int file_write_all(const char *storage_dir, const char *filename,
                   const char *content, size_t content_len) {
    if (!storage_dir || !filename || !content) return -1;
    
    // Ensure files directory exists
    char files_dir[512];
    snprintf(files_dir, sizeof(files_dir), "%s/files", storage_dir);
    mkdir(files_dir, 0755);
    
    char file_path[512];
    snprintf(file_path, sizeof(file_path), "%s/files/%s", storage_dir, filename);
    
    FILE *fp = fopen(file_path, "w");
    if (!fp) {
        return -1;
    }
    
    size_t written = fwrite(content, 1, content_len, fp);
    fclose(fp);
    
    if (written != content_len) {
        return -1;
    }
    
    return 0;
}

// Delete file and metadata
int file_delete(const char *storage_dir, const char *filename) {
    if (!storage_dir || !filename) return -1;
    
    // Build paths
    const char *norm_filename = normalize_filename(filename);
    char file_path[512];
    char meta_path[512];
    snprintf(file_path, sizeof(file_path), "%s/files/%s", storage_dir, norm_filename);
    snprintf(meta_path, sizeof(meta_path), "%s/metadata/%s.meta", storage_dir, norm_filename);
    
    // Delete file (unlink returns 0 on success)
    int file_ok = (unlink(file_path) == 0 || errno == ENOENT);
    
    // Delete metadata (unlink returns 0 on success)
    int meta_ok = (unlink(meta_path) == 0 || errno == ENOENT);
    
    // Return success if at least one deletion succeeded (file might not exist)
    // In practice, if file doesn't exist, we should return error
    // But for robustness, we'll accept if metadata is deleted
    if (!file_ok && !meta_ok) {
        return -1;  // Both failed
    }
    
    return 0;  // Success
}

// Check if file exists
int file_exists(const char *storage_dir, const char *filename) {
    if (!storage_dir || !filename) return 0;
    
    const char *norm_filename = normalize_filename(filename);
    char file_path[512];
    snprintf(file_path, sizeof(file_path), "%s/files/%s", storage_dir, norm_filename);
    
    return (access(file_path, F_OK) == 0) ? 1 : 0;
}

// Load metadata from disk
// Metadata format: Simple text format (can be enhanced to JSON later)
// Format: owner=username\ncreated=timestamp\nlast_modified=timestamp\n...
int metadata_load(const char *storage_dir, const char *filename, FileMetadata *metadata) {
    if (!storage_dir || !filename || !metadata) return -1;
    
    // Build metadata file path
    const char *norm_filename = normalize_filename(filename);
    char meta_path[512];
    snprintf(meta_path, sizeof(meta_path), "%s/metadata/%s.meta", storage_dir, norm_filename);
    
    // Open metadata file
    FILE *fp = fopen(meta_path, "r");
    if (!fp) {
        return -1;  // Metadata file not found
    }
    
    // Initialize metadata structure
    memset(metadata, 0, sizeof(FileMetadata));
    metadata->next_sentence_id = 1;
    
    // Read entire file into buffer for ACL parsing
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    char *file_content = (char *)malloc(file_size + 1);
    if (!file_content) {
        fclose(fp);
        return -1;
    }
    size_t read_bytes = fread(file_content, 1, file_size, fp);
    if (read_bytes != (size_t)file_size && !feof(fp)) {
        free(file_content);
        fclose(fp);
        return -1;
    }
    file_content[read_bytes] = '\0';
    fclose(fp);
    
    // Parse line by line
    char *saveptr = NULL;
    char *line = strtok_r(file_content, "\n", &saveptr);
    while (line) {
        // Parse key=value pairs
        if (strncmp(line, "owner=", 6) == 0) {
            // Copy owner, truncating if too long (owner field is 64 bytes)
            const char *owner_val = line + 6;
            size_t owner_len = strlen(owner_val);
            if (owner_len >= sizeof(metadata->owner)) {
                owner_len = sizeof(metadata->owner) - 1;
            }
            memcpy(metadata->owner, owner_val, owner_len);
            metadata->owner[owner_len] = '\0';
        } else if (strncmp(line, "created=", 8) == 0) {
            metadata->created = (time_t)atoll(line + 8);
        } else if (strncmp(line, "last_modified=", 14) == 0) {
            metadata->last_modified = (time_t)atoll(line + 14);
        } else if (strncmp(line, "last_accessed=", 14) == 0) {
            metadata->last_accessed = (time_t)atoll(line + 14);
        } else if (strncmp(line, "size_bytes=", 11) == 0) {
            metadata->size_bytes = (size_t)atoll(line + 11);
        } else if (strncmp(line, "word_count=", 11) == 0) {
            metadata->word_count = atoi(line + 11);
        } else if (strncmp(line, "char_count=", 11) == 0) {
            metadata->char_count = atoi(line + 11);
        } else if (strncmp(line, "sentence_count=", 15) == 0) {
            int count = atoi(line + 15);
            if (count < 0) count = 0;
            if (count > MAX_SENTENCE_METADATA) count = MAX_SENTENCE_METADATA;
            metadata->sentence_count = count;
        } else if (strncmp(line, "next_sentence_id=", 17) == 0) {
            int next_id = atoi(line + 17);
            if (next_id <= 0) next_id = 1;
            metadata->next_sentence_id = next_id;
        } else if (strncmp(line, "sentence_", 9) == 0) {
            char *eq = strchr(line, '=');
            if (!eq) {
                line = strtok_r(NULL, "\n", &saveptr);
                continue;
            }
            int idx = atoi(line + 9);
            if (idx < 0 || idx >= MAX_SENTENCE_METADATA) {
                line = strtok_r(NULL, "\n", &saveptr);
                continue;
            }
            SentenceMeta *sm = &metadata->sentences[idx];
            int id = 0, version = 0, wcount = 0, ccount = 0;
            size_t offset = 0, length = 0;
            int parsed = sscanf(eq + 1, "%d,%d,%zu,%zu,%d,%d",
                                &id, &version, &offset, &length, &wcount, &ccount);
            if (parsed == 6) {
                sm->sentence_id = id;
                sm->version = version;
                sm->offset = offset;
                sm->length = length;
                sm->word_count = wcount;
                sm->char_count = ccount;
                if (idx + 1 > metadata->sentence_count) {
                    metadata->sentence_count = idx + 1;
                }
                if (metadata->next_sentence_id <= sm->sentence_id) {
                    metadata->next_sentence_id = sm->sentence_id + 1;
                }
            }
        } else if (strncmp(line, "ACL_START", 9) == 0) {
            // ACL section starts - collect all ACL lines
            char acl_buf[4096] = {0};
            size_t acl_pos = 0;
            
            // Collect ACL lines until ACL_END
            while ((line = strtok_r(NULL, "\n", &saveptr))) {
                if (strncmp(line, "ACL_END", 7) == 0) {
                    break;
                }
                // Append line to ACL buffer
                size_t line_len = strlen(line);
                if (acl_pos + line_len + 1 < sizeof(acl_buf)) {
                    memcpy(acl_buf + acl_pos, line, line_len);
                    acl_pos += line_len;
                    acl_buf[acl_pos++] = '\n';
                }
            }
            acl_buf[acl_pos] = '\0';
            
            // Deserialize ACL
            acl_deserialize(&metadata->acl, acl_buf);
            // Don't break - continue reading pending requests
        } else if (strncmp(line, "pending_request_count=", 22) == 0) {
            int count = atoi(line + 22);
            if (count < 0) count = 0;
            if (count > MAX_PENDING_REQUESTS) count = MAX_PENDING_REQUESTS;
            metadata->pending_request_count = count;
        } else if (strncmp(line, "pending_request_", 16) == 0) {
            char *eq = strchr(line, '=');
            if (!eq) {
                line = strtok_r(NULL, "\n", &saveptr);
                continue;
            }
            int idx = atoi(line + 16);
            if (idx < 0 || idx >= MAX_PENDING_REQUESTS) {
                line = strtok_r(NULL, "\n", &saveptr);
                continue;
            }
            PendingRequest *req = &metadata->pending_requests[idx];
            int request_id = 0;
            char requester[64] = {0};
            char access_type = 'R';
            long timestamp = 0;
            
            // Parse: request_id,requester,access_type,timestamp
            char *field_saveptr = NULL;
            char *field = strtok_r(eq + 1, ",", &field_saveptr);
            if (field) request_id = atoi(field);
            field = strtok_r(NULL, ",", &field_saveptr);
            if (field) strncpy(requester, field, sizeof(requester) - 1);
            field = strtok_r(NULL, ",", &field_saveptr);
            if (field) access_type = field[0];
            field = strtok_r(NULL, ",", &field_saveptr);
            if (field) timestamp = atol(field);
            
            req->request_id = request_id;
            size_t req_len = strlen(requester);
            if (req_len >= sizeof(req->requester)) req_len = sizeof(req->requester) - 1;
            memcpy(req->requester, requester, req_len);
            req->requester[req_len] = '\0';
            req->access_type = access_type;
            req->timestamp = (time_t)timestamp;
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }
    
    free(file_content);
    
    // If ACL not found, initialize with owner
    if (metadata->acl.count == 0 && strlen(metadata->owner) > 0) {
        metadata->acl = acl_init(metadata->owner);
    }
    
    return 0;  // Success
}

// Save metadata to disk (atomically)
int metadata_save(const char *storage_dir, const char *filename, const FileMetadata *metadata) {
    if (!storage_dir || !filename || !metadata) return -1;
    
    // Ensure metadata directory exists
    char meta_dir[512];
    snprintf(meta_dir, sizeof(meta_dir), "%s/metadata", storage_dir);
    mkdir(meta_dir, 0755);
    
    // Build paths
    const char *norm_filename = normalize_filename(filename);
    char meta_path[512];
    char tmp_path[520];  // Slightly larger to accommodate ".tmp"
    int n = snprintf(meta_path, sizeof(meta_path), "%s/metadata/%s.meta", storage_dir, norm_filename);
    if (n < 0 || (size_t)n >= sizeof(meta_path)) {
        return -1;  // Path too long
    }
    // Build temp path by appending ".tmp"
    n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", meta_path);
    if (n < 0 || (size_t)n >= sizeof(tmp_path)) {
        return -1;  // Temp path too long
    }
    
    // Write to temporary file first (atomic write)
    FILE *fp = fopen(tmp_path, "w");
    if (!fp) {
        return -1;
    }
    
    // Write metadata in key=value format
    fprintf(fp, "owner=%s\n", metadata->owner);
    fprintf(fp, "created=%ld\n", (long)metadata->created);
    fprintf(fp, "last_modified=%ld\n", (long)metadata->last_modified);
    fprintf(fp, "last_accessed=%ld\n", (long)metadata->last_accessed);
    fprintf(fp, "size_bytes=%zu\n", metadata->size_bytes);
    fprintf(fp, "word_count=%d\n", metadata->word_count);
    fprintf(fp, "char_count=%d\n", metadata->char_count);
    fprintf(fp, "sentence_count=%d\n", metadata->sentence_count);
    fprintf(fp, "next_sentence_id=%d\n", metadata->next_sentence_id);
    for (int i = 0; i < metadata->sentence_count && i < MAX_SENTENCE_METADATA; i++) {
        const SentenceMeta *sm = &metadata->sentences[i];
        fprintf(fp, "sentence_%d=%d,%d,%zu,%zu,%d,%d\n",
                i,
                sm->sentence_id,
                sm->version,
                sm->offset,
                sm->length,
                sm->word_count,
                sm->char_count);
    }
    
    // Write ACL (Step 4)
    fprintf(fp, "ACL_START\n");
    char acl_buf[4096];
    if (acl_serialize(&metadata->acl, acl_buf, sizeof(acl_buf)) == 0) {
        fprintf(fp, "%s", acl_buf);
    }
    fprintf(fp, "ACL_END\n");
    
    // Write pending access requests
    fprintf(fp, "pending_request_count=%d\n", metadata->pending_request_count);
    for (int i = 0; i < metadata->pending_request_count && i < MAX_PENDING_REQUESTS; i++) {
        const PendingRequest *req = &metadata->pending_requests[i];
        fprintf(fp, "pending_request_%d=%d,%s,%c,%ld\n",
                i, req->request_id, req->requester, req->access_type, (long)req->timestamp);
    }
    
    fclose(fp);
    
    // Atomically rename temp file to final file
    // This ensures metadata is never corrupted if SS crashes
    if (rename(tmp_path, meta_path) != 0) {
        unlink(tmp_path);  // Clean up temp file
        return -1;
    }
    
    return 0;  // Success
}

// Update last accessed timestamp
int metadata_update_last_accessed(const char *storage_dir, const char *filename) {
    if (!storage_dir || !filename) return -1;
    
    FileMetadata meta;
    if (metadata_load(storage_dir, filename, &meta) != 0) {
        return -1;  // Can't load metadata
    }
    
    meta.last_accessed = time(NULL);
    return metadata_save(storage_dir, filename, &meta);
}

// Update last modified timestamp
int metadata_update_last_modified(const char *storage_dir, const char *filename) {
    if (!storage_dir || !filename) return -1;
    
    FileMetadata meta;
    if (metadata_load(storage_dir, filename, &meta) != 0) {
        return -1;  // Can't load metadata
    }
    
    meta.last_modified = time(NULL);
    
    // Also update file size and word/char counts
    char content[65536];  // Max 64KB for now
    size_t actual_size;
    if (file_read(storage_dir, filename, content, sizeof(content), &actual_size) == 0) {
        meta.size_bytes = actual_size;
        count_file_stats(content, &meta.word_count, &meta.char_count);
    }
    
    return metadata_save(storage_dir, filename, &meta);
}

// Count words and characters in file content
void count_file_stats(const char *content, int *word_count, int *char_count) {
    if (!content) {
        if (word_count) *word_count = 0;
        if (char_count) *char_count = 0;
        return;
    }
    
    int words = 0;
    int chars = 0;
    int in_word = 0;
    
    // Count characters (including spaces)
    for (const char *p = content; *p; p++) {
        chars++;
        
        // Count words: a word is a sequence of non-space characters
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
            in_word = 0;  // Not in a word
        } else {
            if (!in_word) {
                words++;  // Start of a new word
                in_word = 1;
            }
        }
    }
    
    // Set results
    if (word_count) *word_count = words;
    if (char_count) *char_count = chars;
}

int metadata_ensure_sentences(const char *storage_dir, const char *filename, FileMetadata *metadata) {
    if (!storage_dir || !filename || !metadata) return -1;
    if (metadata->sentence_count > 0) {
        return 0;
    }
    if (metadata->next_sentence_id <= 0) {
        metadata->next_sentence_id = 1;
    }
    metadata->sentence_count = 1;
    metadata->sentences[0].sentence_id = metadata->next_sentence_id++;
    metadata->sentences[0].version = 1;
    metadata->sentences[0].offset = 0;
    metadata->sentences[0].length = metadata->char_count;
    metadata->sentences[0].word_count = metadata->word_count;
    metadata->sentences[0].char_count = metadata->char_count;
    return metadata_save(storage_dir, filename, metadata);
}

int undo_save_state(const char *storage_dir, const char *filename) {
    if (!storage_dir || !filename) return -1;
    char meta_src[512];
    snprintf(meta_src, sizeof(meta_src), "%s/metadata/%s.meta", storage_dir, filename);
    char file_src[512];
    snprintf(file_src, sizeof(file_src), "%s/files/%s", storage_dir, filename);
    char undo_meta[512];
    char undo_data[512];
    build_undo_paths(storage_dir, filename, undo_meta, sizeof(undo_meta),
                     undo_data, sizeof(undo_data));
    if (copy_file_atomic(meta_src, undo_meta) != 0) {
        return -1;
    }
    if (copy_file_atomic(file_src, undo_data) != 0) {
        unlink(undo_meta);
        return -1;
    }
    return 0;
}

int undo_exists(const char *storage_dir, const char *filename) {
    if (!storage_dir || !filename) return 0;
    char undo_meta[512];
    char undo_data[512];
    build_undo_paths(storage_dir, filename, undo_meta, sizeof(undo_meta),
                     undo_data, sizeof(undo_data));
    return (access(undo_meta, F_OK) == 0 && access(undo_data, F_OK) == 0);
}

int undo_restore_state(const char *storage_dir, const char *filename) {
    if (!storage_dir || !filename) return -1;
    char undo_meta[512];
    char undo_data[512];
    build_undo_paths(storage_dir, filename, undo_meta, sizeof(undo_meta),
                     undo_data, sizeof(undo_data));
    if (access(undo_meta, F_OK) != 0 || access(undo_data, F_OK) != 0) {
        return -1;
    }
    char meta_dst[512];
    snprintf(meta_dst, sizeof(meta_dst), "%s/metadata/%s.meta", storage_dir, filename);
    char file_dst[512];
    snprintf(file_dst, sizeof(file_dst), "%s/files/%s", storage_dir, filename);
    if (copy_file_atomic(undo_meta, meta_dst) != 0) {
        return -1;
    }
    if (copy_file_atomic(undo_data, file_dst) != 0) {
        return -1;
    }
    unlink(undo_meta);
    unlink(undo_data);
    return 0;
}

// ===== Folder Operations =====

// Helper function to create nested directories recursively
static int mkdir_recursive(const char *path) {
    char tmp[512];
    char *p = NULL;
    size_t len;
    
    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }
    
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    return mkdir(tmp, 0755);
}

// Create a folder (directory) on the storage server
int folder_create(const char *storage_dir, const char *folder_path) {
    if (!storage_dir || !folder_path) return -1;
    
    // Ensure base directories exist
    ensure_directories(storage_dir);
    
    // Remove leading / from folder_path for building paths
    const char *rel_path = folder_path;
    if (rel_path[0] == '/') rel_path++;
    
    // Build paths for files/ and metadata/ directories
    char files_folder[512];
    char meta_folder[512];
    
    snprintf(files_folder, sizeof(files_folder), "%s/files/%s", storage_dir, rel_path);
    snprintf(meta_folder, sizeof(meta_folder), "%s/metadata/%s", storage_dir, rel_path);
    
    // Create directories (recursive)
    if (mkdir_recursive(files_folder) != 0 && errno != EEXIST) {
        return -1;
    }
    if (mkdir_recursive(meta_folder) != 0 && errno != EEXIST) {
        return -1;
    }
    
    return 0;
}

// Move a file from one folder to another
int file_move(const char *storage_dir, const char *filename,
              const char *old_folder_path, const char *new_folder_path) {
    if (!storage_dir || !filename || !old_folder_path || !new_folder_path) return -1;
    
    // Ensure new folder exists
    if (strcmp(new_folder_path, "/") != 0) {
        folder_create(storage_dir, new_folder_path);
    }
    
    // Build old and new file paths
    char old_file_path[512];
    char new_file_path[512];
    char old_meta_path[512];
    char new_meta_path[512];
    
    // Build full paths (old_folder_path and new_folder_path include leading /)
    const char *old_rel = old_folder_path[0] == '/' ? old_folder_path + 1 : old_folder_path;
    const char *new_rel = new_folder_path[0] == '/' ? new_folder_path + 1 : new_folder_path;
    
    // Handle root folder case
    if (strcmp(old_folder_path, "/") == 0) {
        snprintf(old_file_path, sizeof(old_file_path), "%s/files/%s", storage_dir, filename);
        snprintf(old_meta_path, sizeof(old_meta_path), "%s/metadata/%s.meta", storage_dir, filename);
    } else {
        snprintf(old_file_path, sizeof(old_file_path), "%s/files/%s%s", storage_dir, old_rel, filename);
        snprintf(old_meta_path, sizeof(old_meta_path), "%s/metadata/%s%s.meta", storage_dir, old_rel, filename);
    }
    
    if (strcmp(new_folder_path, "/") == 0) {
        snprintf(new_file_path, sizeof(new_file_path), "%s/files/%s", storage_dir, filename);
        snprintf(new_meta_path, sizeof(new_meta_path), "%s/metadata/%s.meta", storage_dir, filename);
    } else {
        snprintf(new_file_path, sizeof(new_file_path), "%s/files/%s%s", storage_dir, new_rel, filename);
        snprintf(new_meta_path, sizeof(new_meta_path), "%s/metadata/%s%s.meta", storage_dir, new_rel, filename);
    }
    
    // Check if old file exists
    if (access(old_file_path, F_OK) != 0) {
        return -1;  // File doesn't exist
    }
    
    // Move file (rename)
    if (rename(old_file_path, new_file_path) != 0) {
        return -1;
    }
    
    // Move metadata
    if (access(old_meta_path, F_OK) == 0) {
        // Update folder_path in metadata
        FileMetadata meta;
        if (metadata_load(storage_dir, filename, &meta) == 0) {
            strncpy(meta.folder_path, new_folder_path, sizeof(meta.folder_path) - 1);
            meta.folder_path[sizeof(meta.folder_path) - 1] = '\0';
        }
        
        if (rename(old_meta_path, new_meta_path) != 0) {
            // Rollback file move
            rename(new_file_path, old_file_path);
            return -1;
        }
        
        // Save updated metadata with new folder path
        // Note: metadata_save expects filename with folder path
        char full_filename[768];
        if (strcmp(new_folder_path, "/") == 0) {
            snprintf(full_filename, sizeof(full_filename), "%s", filename);
        } else {
            snprintf(full_filename, sizeof(full_filename), "%s%s", new_folder_path, filename);
        }
        metadata_save(storage_dir, full_filename, &meta);
    }
    
    return 0;
}

// ===== Checkpoint Operations =====

// Helper: Validate checkpoint tag (alphanumeric, underscore, dash only)
static int is_valid_checkpoint_tag(const char *tag) {
    if (!tag || strlen(tag) == 0 || strlen(tag) >= 64) return 0;
    
    for (const char *p = tag; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || 
              (*p >= 'A' && *p <= 'Z') || 
              (*p >= '0' && *p <= '9') || 
              *p == '_' || *p == '-')) {
            return 0;
        }
    }
    return 1;
}

// Helper: Build checkpoint directory path
static void build_checkpoint_dir(const char *storage_dir, const char *filename,
                                char *checkpoint_dir, size_t dir_size) {
    const char *norm_filename = normalize_filename(filename);
    snprintf(checkpoint_dir, dir_size, "%s/checkpoints/%s", storage_dir, norm_filename);
}

// Helper: Build checkpoint file paths
static void build_checkpoint_paths(const char *storage_dir, const char *filename, 
                                   const char *tag, char *data_path, size_t data_size,
                                   char *meta_path, size_t meta_size) {
    char checkpoint_dir[600];
    build_checkpoint_dir(storage_dir, filename, checkpoint_dir, sizeof(checkpoint_dir));
    (void)snprintf(data_path, data_size, "%s/%s.checkpoint.data", checkpoint_dir, tag);
    (void)snprintf(meta_path, meta_size, "%s/%s.checkpoint.meta", checkpoint_dir, tag);
}

// Helper: Build checkpoint index path
static void build_checkpoint_index_path(const char *storage_dir, const char *filename,
                                       char *index_path, size_t path_size) {
    char checkpoint_dir[600];
    build_checkpoint_dir(storage_dir, filename, checkpoint_dir, sizeof(checkpoint_dir));
    (void)snprintf(index_path, path_size, "%s/checkpoint.index", checkpoint_dir);
}

// Helper: Load checkpoint index
static int load_checkpoint_index(const char *storage_dir, const char *filename,
                                CheckpointEntry *entries, int *count, int max_count) {
    char index_path[700];
    build_checkpoint_index_path(storage_dir, filename, index_path, sizeof(index_path));
    
    FILE *fp = fopen(index_path, "r");
    if (!fp) {
        *count = 0;
        return 0;  // No index yet, not an error
    }
    
    *count = 0;
    char line[256];
    while (fgets(line, sizeof(line), fp) && *count < max_count) {
        // Format: tag|creator|timestamp|file_size
        char *saveptr = NULL;
        char *tag = strtok_r(line, "|", &saveptr);
        char *creator = strtok_r(NULL, "|", &saveptr);
        char *timestamp_str = strtok_r(NULL, "|", &saveptr);
        char *size_str = strtok_r(NULL, "|", &saveptr);
        
        if (tag && creator && timestamp_str && size_str) {
            strncpy(entries[*count].tag, tag, sizeof(entries[*count].tag) - 1);
            entries[*count].tag[sizeof(entries[*count].tag) - 1] = '\0';
            
            strncpy(entries[*count].creator, creator, sizeof(entries[*count].creator) - 1);
            entries[*count].creator[sizeof(entries[*count].creator) - 1] = '\0';
            
            entries[*count].timestamp = (time_t)atoll(timestamp_str);
            entries[*count].file_size = (size_t)atoll(size_str);
            (*count)++;
        }
    }
    
    fclose(fp);
    return 0;
}

// Helper: Save checkpoint index
static int save_checkpoint_index(const char *storage_dir, const char *filename,
                                const CheckpointEntry *entries, int count) {
    char index_path[700];
    char tmp_path[710];
    build_checkpoint_index_path(storage_dir, filename, index_path, sizeof(index_path));
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", index_path);
    
    FILE *fp = fopen(tmp_path, "w");
    if (!fp) return -1;
    
    for (int i = 0; i < count; i++) {
        fprintf(fp, "%s|%s|%ld|%zu\n",
                entries[i].tag,
                entries[i].creator,
                (long)entries[i].timestamp,
                entries[i].file_size);
    }
    
    fclose(fp);
    
    if (rename(tmp_path, index_path) != 0) {
        unlink(tmp_path);
        return -1;
    }
    
    return 0;
}

int checkpoint_create(const char *storage_dir, const char *filename, 
                     const char *tag, const char *creator) {
    if (!storage_dir || !filename || !tag || !creator) return -1;
    
    // Validate tag
    if (!is_valid_checkpoint_tag(tag)) {
        return -1;
    }
    
    // Check if file exists
    if (!file_exists(storage_dir, filename)) {
        return -1;
    }
    
    // Create checkpoint directory
    char checkpoint_dir[512];
    build_checkpoint_dir(storage_dir, filename, checkpoint_dir, sizeof(checkpoint_dir));
    mkdir_recursive(checkpoint_dir);
    
    // Load existing checkpoint index
    CheckpointEntry entries[MAX_CHECKPOINTS_PER_FILE];
    int count = 0;
    load_checkpoint_index(storage_dir, filename, entries, &count, MAX_CHECKPOINTS_PER_FILE);
    
    // Check if tag already exists
    for (int i = 0; i < count; i++) {
        if (strcmp(entries[i].tag, tag) == 0) {
            return -1;  // Tag already exists
        }
    }
    
    // Check if we've reached the limit
    if (count >= MAX_CHECKPOINTS_PER_FILE) {
        return -1;  // Too many checkpoints
    }
    
    // Build paths
    const char *norm_filename = normalize_filename(filename);
    char src_file[512], src_meta[512];
    char dst_data[700], dst_meta[700];
    
    snprintf(src_file, sizeof(src_file), "%s/files/%s", storage_dir, norm_filename);
    snprintf(src_meta, sizeof(src_meta), "%s/metadata/%s.meta", storage_dir, norm_filename);
    build_checkpoint_paths(storage_dir, filename, tag, dst_data, sizeof(dst_data),
                          dst_meta, sizeof(dst_meta));
    
    // Copy file content
    if (copy_file_atomic(src_file, dst_data) != 0) {
        return -1;
    }
    
    // Copy metadata
    if (copy_file_atomic(src_meta, dst_meta) != 0) {
        unlink(dst_data);
        return -1;
    }
    
    // Get file size
    struct stat st;
    size_t file_size = 0;
    if (stat(src_file, &st) == 0) {
        file_size = st.st_size;
    }
    
    // Add to index
    strncpy(entries[count].tag, tag, sizeof(entries[count].tag) - 1);
    entries[count].tag[sizeof(entries[count].tag) - 1] = '\0';
    
    strncpy(entries[count].creator, creator, sizeof(entries[count].creator) - 1);
    entries[count].creator[sizeof(entries[count].creator) - 1] = '\0';
    
    entries[count].timestamp = time(NULL);
    entries[count].file_size = file_size;
    count++;
    
    // Save index
    if (save_checkpoint_index(storage_dir, filename, entries, count) != 0) {
        // Cleanup
        unlink(dst_data);
        unlink(dst_meta);
        return -1;
    }
    
    return 0;
}

int checkpoint_exists(const char *storage_dir, const char *filename, const char *tag) {
    if (!storage_dir || !filename || !tag) return 0;
    
    char data_path[700], meta_path[700];
    build_checkpoint_paths(storage_dir, filename, tag, data_path, sizeof(data_path),
                          meta_path, sizeof(meta_path));
    
    return (access(data_path, F_OK) == 0 && access(meta_path, F_OK) == 0) ? 1 : 0;
}

int checkpoint_restore(const char *storage_dir, const char *filename, const char *tag) {
    if (!storage_dir || !filename || !tag) return -1;
    
    // Check if checkpoint exists
    if (!checkpoint_exists(storage_dir, filename, tag)) {
        return -1;
    }
    
    // Build paths
    const char *norm_filename = normalize_filename(filename);
    char src_data[700], src_meta[700];
    char dst_file[512], dst_meta[512];
    
    build_checkpoint_paths(storage_dir, filename, tag, src_data, sizeof(src_data),
                          src_meta, sizeof(src_meta));
    snprintf(dst_file, sizeof(dst_file), "%s/files/%s", storage_dir, norm_filename);
    snprintf(dst_meta, sizeof(dst_meta), "%s/metadata/%s.meta", storage_dir, norm_filename);
    
    // Restore file content
    if (copy_file_atomic(src_data, dst_file) != 0) {
        return -1;
    }
    
    // Restore metadata
    if (copy_file_atomic(src_meta, dst_meta) != 0) {
        return -1;
    }
    
    return 0;
}

int checkpoint_list(const char *storage_dir, const char *filename, 
                   CheckpointEntry **entries, int *count) {
    if (!storage_dir || !filename || !entries || !count) return -1;
    
    // Allocate array for entries
    CheckpointEntry *arr = (CheckpointEntry *)malloc(sizeof(CheckpointEntry) * MAX_CHECKPOINTS_PER_FILE);
    if (!arr) return -1;
    
    // Load index
    if (load_checkpoint_index(storage_dir, filename, arr, count, MAX_CHECKPOINTS_PER_FILE) != 0) {
        free(arr);
        return -1;
    }
    
    *entries = arr;
    return 0;
}

int checkpoint_get_content(const char *storage_dir, const char *filename, 
                          const char *tag, char *content_buf, 
                          size_t buf_size, size_t *actual_size) {
    if (!storage_dir || !filename || !tag || !content_buf) return -1;
    
    // Check if checkpoint exists
    if (!checkpoint_exists(storage_dir, filename, tag)) {
        return -1;
    }
    
    // Build checkpoint data path
    char data_path[700], meta_path[700];
    build_checkpoint_paths(storage_dir, filename, tag, data_path, sizeof(data_path),
                          meta_path, sizeof(meta_path));
    
    // Read checkpoint file
    FILE *fp = fopen(data_path, "rb");
    if (!fp) return -1;
    
    size_t bytes_read = fread(content_buf, 1, buf_size - 1, fp);
    fclose(fp);
    
    content_buf[bytes_read] = '\0';
    
    if (actual_size) {
        *actual_size = bytes_read;
    }
    
    return 0;
}

