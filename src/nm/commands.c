#define _POSIX_C_SOURCE 200809L
#include "commands.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

#include "../common/net.h"
#include "../common/log.h"
#include "registry.h"
#include "access_requests.h"
#include "replication.h"
#include "replication_worker.h"
#include "heartbeat_monitor.h"

#define MAX_SS_CANDIDATES 64
#define ACL_CACHE_CAPACITY 256
#define ACL_CACHE_KEY_MAX 768

typedef struct {
    char path[ACL_CACHE_KEY_MAX];
    ACL acl;
    int valid;
} AclCacheEntry;

static AclCacheEntry g_acl_cache[ACL_CACHE_CAPACITY];
static int g_acl_cache_next_slot = 0;
static pthread_mutex_t g_acl_cache_mu = PTHREAD_MUTEX_INITIALIZER;

static int get_ss_connection_for_file(const FileEntry *entry);
static int fetch_file_content_from_ss(const FileEntry *entry, char **content_out);
static int execute_script_text(const char *script_text, char **output_out, char *error_buf, size_t error_len);
static int send_streaming_response(int client_fd, const char *id, const char *username, const char *text);

// Helper: Get active SS host/port for file (primary if alive, else replica)
static void get_active_ss_for_file(const FileEntry *entry, 
                                   const char **host, int *port, const char **ss_name) {
    // Check if primary is alive
    if (heartbeat_monitor_is_alive(entry->ss_username)) {
        *host = entry->ss_host;
        *port = entry->ss_client_port;
        *ss_name = entry->ss_username;
    } else if (entry->replica_ss_username[0] != '\0' && 
               heartbeat_monitor_is_alive(entry->replica_ss_username)) {
        // Primary failed, use replica
        *host = entry->replica_ss_host;
        *port = entry->replica_ss_client_port;
        *ss_name = entry->replica_ss_username;
        log_info("nm_failover_read", "Primary %s failed, using replica %s", 
                 entry->ss_username, entry->replica_ss_username);
    } else {
        // Both failed or no replica - use primary anyway (will fail)
        *host = entry->ss_host;
        *port = entry->ss_client_port;
        *ss_name = entry->ss_username;
    }
}

static void build_full_path(const FileEntry *entry, char *full_path, size_t len) {
    if (!entry || !full_path || len == 0) return;
    if (strcmp(entry->folder_path, "/") == 0) {
        snprintf(full_path, len, "%s", entry->filename);
    } else {
        size_t folder_len = strlen(entry->folder_path);
        if (folder_len > 0 && entry->folder_path[folder_len - 1] == '/') {
            snprintf(full_path, len, "%.*s/%s",
                     (int)(folder_len - 1), entry->folder_path, entry->filename);
        } else {
            snprintf(full_path, len, "%s/%s", entry->folder_path, entry->filename);
        }
    }
}

static int acl_cache_get(const char *path, ACL *acl_out) {
    if (!path || !acl_out) return -1;
    pthread_mutex_lock(&g_acl_cache_mu);
    for (int i = 0; i < ACL_CACHE_CAPACITY; i++) {
        if (g_acl_cache[i].valid && strcmp(g_acl_cache[i].path, path) == 0) {
            *acl_out = g_acl_cache[i].acl;
            pthread_mutex_unlock(&g_acl_cache_mu);
            return 0;
        }
    }
    pthread_mutex_unlock(&g_acl_cache_mu);
    return -1;
}

static void acl_cache_put(const char *path, const ACL *acl) {
    if (!path || !acl) return;
    pthread_mutex_lock(&g_acl_cache_mu);
    // Update existing entry if present
    for (int i = 0; i < ACL_CACHE_CAPACITY; i++) {
        if (g_acl_cache[i].valid && strcmp(g_acl_cache[i].path, path) == 0) {
            g_acl_cache[i].acl = *acl;
            pthread_mutex_unlock(&g_acl_cache_mu);
            return;
        }
    }
    AclCacheEntry *slot = &g_acl_cache[g_acl_cache_next_slot];
    snprintf(slot->path, sizeof(slot->path), "%s", path);
    slot->acl = *acl;
    slot->valid = 1;
    g_acl_cache_next_slot = (g_acl_cache_next_slot + 1) % ACL_CACHE_CAPACITY;
    pthread_mutex_unlock(&g_acl_cache_mu);
}

static void acl_cache_invalidate(const char *path) {
    if (!path) return;
    pthread_mutex_lock(&g_acl_cache_mu);
    for (int i = 0; i < ACL_CACHE_CAPACITY; i++) {
        if (g_acl_cache[i].valid && strcmp(g_acl_cache[i].path, path) == 0) {
            g_acl_cache[i].valid = 0;
        }
    }
    pthread_mutex_unlock(&g_acl_cache_mu);
}

// Helper: Fetch ACL from storage server (with cache)
static int fetch_acl_from_ss(const FileEntry *entry, ACL *acl_out) {
    if (!entry || !acl_out) return -1;

    char full_path[ACL_CACHE_KEY_MAX];
    build_full_path(entry, full_path, sizeof(full_path));
    if (acl_cache_get(full_path, acl_out) == 0) {
        return 0;
    }

    int ss_fd = get_ss_connection_for_file(entry);
    if (ss_fd < 0) {
        return -1;
    }

    // Send GET_ACL command
    Message req = {0};
    (void)snprintf(req.type, sizeof(req.type), "%s", "GET_ACL");
    (void)snprintf(req.id, sizeof(req.id), "%s", "1");
    (void)snprintf(req.username, sizeof(req.username), "%s", "NM");
    (void)snprintf(req.role, sizeof(req.role), "%s", "NM");
    
    // Build full path: folder_path + filename
    (void)snprintf(req.payload, sizeof(req.payload), "%s", full_path);

    char req_buf[MAX_LINE];
    if (proto_format_line(&req, req_buf, sizeof(req_buf)) != 0) {
        close(ss_fd);
        return -1;
    }
    if (send_all(ss_fd, req_buf, strlen(req_buf)) != 0) {
        close(ss_fd);
        return -1;
    }

    // Receive response
    char resp_buf[MAX_LINE];
    int n = recv_line(ss_fd, resp_buf, sizeof(resp_buf));
    close(ss_fd);
    if (n <= 0) {
        return -1;
    }

    Message resp;
    if (proto_parse_line(resp_buf, &resp) != 0) {
        return -1;
    }

    if (strcmp(resp.type, "ERROR") == 0) {
        return -1;
    }

    if (strcmp(resp.type, "ACL") != 0) {
        return -1;
    }

    // Convert \x01 back to \n
    char acl_serialized[4096];
    size_t pos = 0;
    for (size_t i = 0; i < strlen(resp.payload) && pos < sizeof(acl_serialized) - 1; i++) {
        if (resp.payload[i] == '\x01') {
            acl_serialized[pos++] = '\n';
        } else {
            acl_serialized[pos++] = resp.payload[i];
        }
    }
    acl_serialized[pos] = '\0';

    if (acl_deserialize(acl_out, acl_serialized) != 0) {
        return -1;
    }

    acl_cache_put(full_path, acl_out);
    return 0;
}

static int fetch_file_content_from_ss(const FileEntry *entry, char **content_out) {
    if (!entry || !content_out) return -1;
    int ss_fd = get_ss_connection_for_file(entry);
    if (ss_fd < 0) return -1;

    Message req = {0};
    (void)snprintf(req.type, sizeof(req.type), "%s", "GET_FILE");
    (void)snprintf(req.id, sizeof(req.id), "%s", "1");
    (void)snprintf(req.username, sizeof(req.username), "%s", "NM");
    (void)snprintf(req.role, sizeof(req.role), "%s", "NM");
    (void)snprintf(req.payload, sizeof(req.payload), "%s", entry->filename);

    char req_buf[MAX_LINE];
    if (proto_format_line(&req, req_buf, sizeof(req_buf)) != 0 ||
        send_all(ss_fd, req_buf, strlen(req_buf)) != 0) {
        close(ss_fd);
        return -1;
    }

    size_t cap = 4096;
    size_t len = 0;
    char *buffer = (char*)malloc(cap);
    if (!buffer) {
        close(ss_fd);
        return -1;
    }

    while (1) {
        char resp_buf[MAX_LINE];
        int n = recv_line(ss_fd, resp_buf, sizeof(resp_buf));
        if (n <= 0) {
            free(buffer);
            close(ss_fd);
            return -1;
        }
        Message resp;
        if (proto_parse_line(resp_buf, &resp) != 0) {
            free(buffer);
            close(ss_fd);
            return -1;
        }
        if (strcmp(resp.type, "ERROR") == 0) {
            free(buffer);
            close(ss_fd);
            return -1;
        }
        if (strcmp(resp.type, "STOP") == 0) {
            break;
        }
        if (strcmp(resp.type, "DATA") == 0) {
            for (size_t i = 0; i < strlen(resp.payload); i++) {
                char c = (resp.payload[i] == '\x01') ? '\n' : resp.payload[i];
                if (len + 1 >= cap) {
                    cap *= 2;
                    char *tmp = realloc(buffer, cap);
                    if (!tmp) {
                        free(buffer);
                        close(ss_fd);
                        return -1;
                    }
                    buffer = tmp;
                }
                buffer[len++] = c;
            }
        }
    }
    close(ss_fd);
    if (len + 1 >= cap) {
        char *tmp = realloc(buffer, len + 1);
        if (!tmp) {
            free(buffer);
            return -1;
        }
        buffer = tmp;
    }
    buffer[len] = '\0';
    *content_out = buffer;
    return 0;
}

static int execute_script_text(const char *script_text, char **output_out, char *error_buf, size_t error_len) {
    if (!script_text || !output_out) return -1;
    char tmp_template[] = "/tmp/langexecXXXXXX";
    int fd = mkstemp(tmp_template);
    if (fd < 0) {
        if (error_buf && error_len > 0) snprintf(error_buf, error_len, "mkstemp failed");
        return -1;
    }
    size_t script_len = strlen(script_text);
    if (write(fd, script_text, script_len) != (ssize_t)script_len) {
        if (error_buf && error_len > 0) snprintf(error_buf, error_len, "write failed");
        close(fd);
        unlink(tmp_template);
        return -1;
    }
    close(fd);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "/bin/sh %s 2>&1", tmp_template);
    FILE *pipe = popen(cmd, "r");
    if (!pipe) {
        if (error_buf && error_len > 0) snprintf(error_buf, error_len, "popen failed");
        unlink(tmp_template);
        return -1;
    }

    size_t cap = 4096;
    size_t len = 0;
    char *buffer = (char*)malloc(cap);
    if (!buffer) {
        pclose(pipe);
        unlink(tmp_template);
        return -1;
    }
    char chunk[512];
    while (fgets(chunk, sizeof(chunk), pipe)) {
        size_t chunk_len = strlen(chunk);
        if (len + chunk_len + 1 >= cap) {
            cap = (len + chunk_len + 1) * 2;
            char *tmp = realloc(buffer, cap);
            if (!tmp) {
                free(buffer);
                pclose(pipe);
                unlink(tmp_template);
                return -1;
            }
            buffer = tmp;
        }
        memcpy(buffer + len, chunk, chunk_len);
        len += chunk_len;
    }
    buffer[len] = '\0';
    pclose(pipe);
    unlink(tmp_template);
    *output_out = buffer;
    return 0;
}

static int send_streaming_response(int client_fd, const char *id, const char *username, const char *text) {
    if (!text) text = "";
    size_t len = strlen(text);
    size_t pos = 0;
    while (pos < len) {
        Message resp = {0};
        (void)snprintf(resp.type, sizeof(resp.type), "%s", "DATA");
        (void)snprintf(resp.id, sizeof(resp.id), "%s", id ? id : "");
        (void)snprintf(resp.username, sizeof(resp.username), "%s", username ? username : "");
        (void)snprintf(resp.role, sizeof(resp.role), "%s", "NM");
        size_t payload_pos = 0;
        size_t payload_max = sizeof(resp.payload) - 1;
        while (pos < len && payload_pos < payload_max) {
            char c = text[pos++];
            resp.payload[payload_pos++] = (c == '\n') ? '\x01' : c;
        }
        resp.payload[payload_pos] = '\0';
        char line[MAX_LINE];
        if (proto_format_line(&resp, line, sizeof(line)) != 0 ||
            send_all(client_fd, line, strlen(line)) != 0) {
            return -1;
        }
    }
    Message stop = {0};
    (void)snprintf(stop.type, sizeof(stop.type), "%s", "STOP");
    (void)snprintf(stop.id, sizeof(stop.id), "%s", id ? id : "");
    (void)snprintf(stop.username, sizeof(stop.username), "%s", username ? username : "");
    (void)snprintf(stop.role, sizeof(stop.role), "%s", "NM");
    char stop_line[MAX_LINE];
    if (proto_format_line(&stop, stop_line, sizeof(stop_line)) != 0 ||
        send_all(client_fd, stop_line, strlen(stop_line)) != 0) {
        return -1;
    }
    return 0;
}
// Helper: Send error response
int send_error_response(int client_fd, const char *id, const char *username,
                       const Error *error) {
    if (!error) return -1;
    
    const char *error_code = error_code_to_string(error->code);
    const char *error_msg = error->message;
    
    char resp_buf[MAX_LINE];
    if (proto_format_error(id, username, "NM", error_code, error_msg,
                          resp_buf, sizeof(resp_buf)) != 0) {
        return -1;
    }
    
    return send_all(client_fd, resp_buf, strlen(resp_buf));
}

// Helper: Send success response
int send_success_response(int client_fd, const char *id, const char *username,
                          const char *message) {
    Message resp = {0};
    (void)snprintf(resp.type, sizeof(resp.type), "%s", "ACK");
    (void)snprintf(resp.id, sizeof(resp.id), "%s", id ? id : "");
    (void)snprintf(resp.username, sizeof(resp.username), "%s", username ? username : "");
    (void)snprintf(resp.role, sizeof(resp.role), "%s", "NM");
    (void)snprintf(resp.payload, sizeof(resp.payload), "%s", message ? message : "");
    
    char resp_buf[MAX_LINE];
    if (proto_format_line(&resp, resp_buf, sizeof(resp_buf)) != 0) {
        return -1;
    }
    
    return send_all(client_fd, resp_buf, strlen(resp_buf));
}

// Helper: Send data response
int send_data_response(int client_fd, const char *id, const char *username,
                      const char *data) {
    Message resp = {0};
    (void)snprintf(resp.type, sizeof(resp.type), "%s", "DATA");
    (void)snprintf(resp.id, sizeof(resp.id), "%s", id ? id : "");
    (void)snprintf(resp.username, sizeof(resp.username), "%s", username ? username : "");
    (void)snprintf(resp.role, sizeof(resp.role), "%s", "NM");
    
    // Copy data to payload, ensuring it fits (payload is 1792 bytes)
    // Replace newlines with \x01 (SOH - Start of Heading) to avoid breaking the line-based protocol
    // This character is unlikely to appear in normal text and won't be stripped by the parser
    // Client will convert \x01 back to \n
    if (data && strlen(data) > 0) {
        size_t data_len = strlen(data);
        size_t payload_pos = 0;
        size_t payload_max = sizeof(resp.payload) - 1;
        
        for (size_t i = 0; i < data_len && payload_pos < payload_max; i++) {
            if (data[i] == '\n') {
                // Replace newline with \x01 (SOH) as placeholder
                if (payload_pos + 1 < payload_max) {
                    resp.payload[payload_pos++] = '\x01';
                }
            } else {
                resp.payload[payload_pos++] = data[i];
            }
        }
        resp.payload[payload_pos] = '\0';
    } else {
        resp.payload[0] = '\0';
    }
    
    char resp_buf[MAX_LINE];
    if (proto_format_line(&resp, resp_buf, sizeof(resp_buf)) != 0) {
        log_error("nm_send_data_fmt", "failed to format response");
        return -1;
    }
    
    int result = send_all(client_fd, resp_buf, strlen(resp_buf));
    if (result != 0) {
        log_error("nm_send_data_send", "failed to send response");
    }
    return result;
}

// Helper: Get SS connection for a file
// Returns: SS connection fd, or -1 on error
static int get_ss_connection_for_file(const FileEntry *entry) {
    if (!entry) return -1;
    
    // Get active SS (failover to replica if primary is down)
    const char *active_host;
    int active_port;
    const char *active_ss_name;
    get_active_ss_for_file(entry, &active_host, &active_port, &active_ss_name);
    
    // Connect to active SS
    int fd = connect_to_host(active_host, active_port);
    if (fd < 0) {
        return -1;
    }
    
    return fd;
}

// Helper: Find SS by username (for CREATE - need to select SS)
// Returns: SS connection fd, or -1 on error
static int find_ss_connection(const char *ss_username) {
    if (!ss_username) return -1;
    
    // Get SS info from registry
    char ss_host[64] = {0};
    int ss_client_port = 0;
    
    if (registry_get_ss_info(ss_username, ss_host, sizeof(ss_host), &ss_client_port) != 0) {
        return -1;
    }
    
    // Connect to SS
    if (strlen(ss_host) > 0 && ss_client_port > 0) {
        return connect_to_host(ss_host, ss_client_port);
    }
    return -1;
}

// Helper: Load owner from SS metadata if not already set
// Returns: 0 on success, -1 on error
static int load_owner_from_ss(FileEntry *entry) {
    if (!entry) return -1;
    
    // If owner is already set (non-empty), no need to load
    if (entry->owner[0] != '\0') {
        return 0;  // Already has owner
    }
    
    char full_path[ACL_CACHE_KEY_MAX];
    build_full_path(entry, full_path, sizeof(full_path));

    // Connect to SS
    int ss_fd = get_ss_connection_for_file(entry);
    if (ss_fd < 0) {
        log_error("nm_load_owner", "Cannot connect to SS for file=%s", entry->filename);
        return -1;
    }
    
    // Send GETMETA command to SS to get metadata
    Message meta_cmd = {0};
    (void)snprintf(meta_cmd.type, sizeof(meta_cmd.type), "%s", "GETMETA");
    (void)snprintf(meta_cmd.id, sizeof(meta_cmd.id), "%s", "1");
    (void)snprintf(meta_cmd.username, sizeof(meta_cmd.username), "%s", "NM");
    (void)snprintf(meta_cmd.role, sizeof(meta_cmd.role), "%s", "NM");
    (void)snprintf(meta_cmd.payload, sizeof(meta_cmd.payload), "%s", entry->filename);
    
    char cmd_line[MAX_LINE];
    proto_format_line(&meta_cmd, cmd_line, sizeof(cmd_line));
    if (send_all(ss_fd, cmd_line, strlen(cmd_line)) != 0) {
        close(ss_fd);
        log_error("nm_load_owner", "Failed to send GETMETA to SS");
        return -1;
    }
    
    // Wait for response from SS
    char resp_buf[MAX_LINE];
    int n = recv_line(ss_fd, resp_buf, sizeof(resp_buf));
    close(ss_fd);
    
    if (n <= 0) {
        log_error("nm_load_owner", "No response from SS");
        return -1;
    }
    
    Message ss_resp;
    if (proto_parse_line(resp_buf, &ss_resp) != 0) {
        log_error("nm_load_owner", "Invalid response from SS");
        return -1;
    }
    
    // Check if SS returned error
    if (strcmp(ss_resp.type, "ERROR") == 0) {
        log_error("nm_load_owner", "SS returned error for file=%s", entry->filename);
        return -1;
    }
    
    // Parse metadata from payload: "owner=alice,size=100,..."
    char *owner_start = strstr(ss_resp.payload, "owner=");
    if (owner_start) {
        owner_start += 6;  // Skip "owner="
        char *owner_end = strchr(owner_start, ',');
        if (owner_end) {
            size_t owner_len = owner_end - owner_start;
            if (owner_len < sizeof(entry->owner)) {
                memcpy(entry->owner, owner_start, owner_len);
                entry->owner[owner_len] = '\0';
                log_info("nm_owner_loaded", "file=%s owner=%s", entry->filename, entry->owner);
                return 0;
            }
        } else {
            // No comma after owner (it's the last field or only field)
            strncpy(entry->owner, owner_start, sizeof(entry->owner) - 1);
            entry->owner[sizeof(entry->owner) - 1] = '\0';
            log_info("nm_owner_loaded", "file=%s owner=%s", entry->filename, entry->owner);
            return 0;
        }
    }
    
    log_error("nm_load_owner", "Owner not found in metadata for file=%s", entry->filename);
    return -1;
}

// Helper: Select SS for new file (round-robin or first available)
// Returns: SS username, or NULL if no SS available
// Handle VIEW command
int handle_view(int client_fd, const char *username, const char *flags) {
    // printf("DEBUG: handle_view called with username=%s flags=%s\n", username, flags);
    if (!username || !client_fd) {
        return -1;
    }
    
    // Parse flags
    int show_all = 0;  // -a flag
    int show_details = 0;  // -l flag
    
    if (flags) {
        if (strchr(flags, 'a')) show_all = 1;
        if (strchr(flags, 'l')) show_details = 1;
    }
    
    // Get files from index
    FileEntry *all_files[1000];
    int total_count = index_get_all_files(all_files, 1000);
    
    // Filter files based on access (if not -a)
    FileEntry *filtered_files[1000];
    int filtered_count = 0;
    
    // Load owner from SS metadata for files that don't have it set
    // printf("DEBUG: SHOW_ALL :%d total_count=%d\n", show_all, total_count);
    for (int i = 0; i < total_count; i++) {
        if (all_files[i]->owner[0] == '\0') {
            // Owner not set - load from SS metadata
            load_owner_from_ss(all_files[i]);
        }
    }
    
    if (show_all) {
        // Show all files
        for (int i = 0; i < total_count; i++) {
            filtered_files[filtered_count++] = all_files[i];
        }
    } else {
        // Show files owned by user
        // printf("DEBUG: %d\n",total_count);;
        // fflush(stdout);
        for (int i = 0; i < total_count; i++) {
            log_info("nm_view_check_owner", "file=%s owner=%s user=%s",
                     all_files[i]->filename, all_files[i]->owner, username);
            if (strcmp(all_files[i]->owner, username) == 0) {
                filtered_files[filtered_count++] = all_files[i];
            }
        }
    }
    
    // Format output
    char output[8192] = {0};
    size_t output_pos = 0;
    
    if (show_details) {
        // Table format with details
        (void)snprintf(output + output_pos, sizeof(output) - output_pos,
                      "---------------------------------------------------------\n"
                      "|  Filename  | Words | Chars | Last Access Time | Owner |\n"
                      "|------------|-------|-------|------------------|-------|\n");
        output_pos = strlen(output);
        
        for (int i = 0; i < filtered_count && output_pos < sizeof(output) - 200; i++) {
            FileEntry *f = filtered_files[i];
            char time_str[32];
            struct tm *tm = localtime(&f->last_accessed);
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M", tm);
            
            // Build full path for display
            char full_path[768];
            if (strcmp(f->folder_path, "/") == 0) {
                snprintf(full_path, sizeof(full_path), "%s", f->filename);
            } else {
                size_t folder_len = strlen(f->folder_path);
                if (folder_len > 0 && f->folder_path[folder_len - 1] == '/') {
                    snprintf(full_path, sizeof(full_path), "%.*s/%s", 
                             (int)(folder_len - 1), f->folder_path, f->filename);
                } else {
                    snprintf(full_path, sizeof(full_path), "%s/%s", f->folder_path, f->filename);
                }
            }
            
            int n = snprintf(output + output_pos, sizeof(output) - output_pos,
                           "| %-10s | %5d | %5d | %-16s | %-5s |\n",
                           full_path, f->word_count, f->char_count,
                           time_str, f->owner);
            if (n > 0) output_pos += n;
        }
        
        (void)snprintf(output + output_pos, sizeof(output) - output_pos,
                      "---------------------------------------------------------\n");
    } else {
        // Simple list format - show full path
        for (int i = 0; i < filtered_count && output_pos < sizeof(output) - 100; i++) {
            // Build full path: folder_path + filename
            char full_path[768];
            if (strcmp(filtered_files[i]->folder_path, "/") == 0) {
                snprintf(full_path, sizeof(full_path), "%s", filtered_files[i]->filename);
            } else {
                // Remove trailing / from folder_path if present for display
                size_t folder_len = strlen(filtered_files[i]->folder_path);
                if (folder_len > 0 && filtered_files[i]->folder_path[folder_len - 1] == '/') {
                    snprintf(full_path, sizeof(full_path), "%.*s/%s", 
                             (int)(folder_len - 1), filtered_files[i]->folder_path, 
                             filtered_files[i]->filename);
                } else {
                    snprintf(full_path, sizeof(full_path), "%s/%s", 
                             filtered_files[i]->folder_path, filtered_files[i]->filename);
                }
            }
            
            int n = snprintf(output + output_pos, sizeof(output) - output_pos,
                           "--> %s\n", full_path);
            if (n > 0) {
                output_pos += n;
            } else {
                // snprintf failed or truncated
                break;
            }
        }
        // Ensure output is null-terminated
        output[output_pos] = '\0';
    }
    
    // If no files found, show a message
    if (filtered_count == 0) {
        if (show_all) {
            (void)snprintf(output, sizeof(output), "No files found.\n");
        } else {
            (void)snprintf(output, sizeof(output), "No files found. (Use -a to view all files)\n");
        }
    }
    
    // Send response
    return send_data_response(client_fd, "", username, output);
}

// Handle CREATE command
int handle_create(int client_fd, const char *username, const char *filename) {
    if (!username || !filename || !client_fd) {
        Error err = error_simple(ERR_INVALID, "Invalid parameters");
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Check if file already exists
    FileEntry *existing = index_lookup_file(filename);
    if (existing) {
        Error err = error_create(ERR_CONFLICT, "File '%s' already exists", filename);
        return send_error_response(client_fd, "", username, &err);
    }
    
    char ss_candidates[MAX_SS_CANDIDATES][64] = {{0}};
    int ss_count = registry_get_ss_candidates(ss_candidates, MAX_SS_CANDIDATES);
    if (ss_count <= 0) {
        Error err = error_simple(ERR_UNAVAILABLE, "No storage server available");
        return send_error_response(client_fd, "", username, &err);
    }

    char selected_ss[64] = {0};
    Message ss_resp = {0};

    for (int i = 0; i < ss_count; i++) {
        const char *candidate = ss_candidates[i];
        int ss_fd = find_ss_connection(candidate);
        if (ss_fd < 0) {
            log_error("nm_create_connect", "Cannot reach SS %s", candidate);
            continue;
        }

        Message create_cmd = {0};
        (void)snprintf(create_cmd.type, sizeof(create_cmd.type), "%s", "CREATE");
        (void)snprintf(create_cmd.id, sizeof(create_cmd.id), "%s", "1");
        (void)snprintf(create_cmd.username, sizeof(create_cmd.username), "%s", username);
        (void)snprintf(create_cmd.role, sizeof(create_cmd.role), "%s", "NM");
        (void)snprintf(create_cmd.payload, sizeof(create_cmd.payload), "%s", filename);

        char cmd_line[MAX_LINE];
        proto_format_line(&create_cmd, cmd_line, sizeof(cmd_line));
        if (send_all(ss_fd, cmd_line, strlen(cmd_line)) != 0) {
            log_error("nm_create_send", "Failed to send CREATE to SS %s", candidate);
            close(ss_fd);
            continue;
        }

        char resp_buf[MAX_LINE];
        int n = recv_line(ss_fd, resp_buf, sizeof(resp_buf));
        close(ss_fd);

        if (n <= 0) {
            log_error("nm_create_resp", "No response from SS %s", candidate);
            continue;
        }

        if (proto_parse_line(resp_buf, &ss_resp) != 0) {
            log_error("nm_create_resp", "Invalid response from SS %s", candidate);
            continue;
        }

        if (strcmp(ss_resp.type, "ERROR") == 0) {
            char error_code[64];
            char error_msg[256];
            proto_parse_error(&ss_resp, error_code, sizeof(error_code),
                              error_msg, sizeof(error_msg));
            Error err = error_simple(ERR_INTERNAL, error_msg);
            return send_error_response(client_fd, "", username, &err);
        }

        strncpy(selected_ss, candidate, sizeof(selected_ss) - 1);
        selected_ss[sizeof(selected_ss) - 1] = '\0';
        break;
    }

    if (selected_ss[0] == '\0') {
        Error err = error_simple(ERR_UNAVAILABLE, "Cannot connect to any storage server");
        return send_error_response(client_fd, "", username, &err);
    }
    
    // SS created file successfully - add to index or update existing entry
    // Get SS info for index
    FileEntry *entry = index_lookup_file(filename);
    
    // Get SS info from registry
    char ss_host[64] = {0};
    int ss_client_port = 0;
    
    if (registry_get_ss_info(selected_ss, ss_host, sizeof(ss_host), &ss_client_port) == 0) {
        if (!entry) {
            // File not in index - add it
            entry = index_add_file(filename, username, ss_host, ss_client_port, selected_ss);
        } else {
            // File exists in index (probably from SS registration with owner=ss1)
            // Update the owner to the actual creator
            size_t owner_len = strlen(username);
            size_t copy_len = (owner_len < sizeof(entry->owner) - 1) ? owner_len : sizeof(entry->owner) - 1;
            memcpy(entry->owner, username, copy_len);
            entry->owner[copy_len] = '\0';
            log_info("nm_file_owner_updated", "file=%s new_owner=%s", filename, username);
        }
    }
    
    if (entry) {
        // Auto-register folder if file has a folder path
        if (strcmp(entry->folder_path, "/") != 0) {
            index_add_folder(entry->folder_path, selected_ss);
        }
        
        log_info("nm_file_created", "file=%s owner=%s", filename, entry->owner);
        registry_adjust_ss_file_count(selected_ss, 1);
        
        // Queue async replication to backup SS
        const char *replica = replication_get_replica(selected_ss);
        log_info("nm_replication_check", "file=%s primary=%s replica=%s", 
                 filename, selected_ss, replica ? replica : "NULL");
        if (replica) {
            int queue_result = replication_worker_queue(REPL_OP_CREATE, filename, selected_ss, replica);
            log_info("nm_replication_queue_result", "file=%s result=%d", filename, queue_result);
            if (queue_result == 0) {
                log_info("nm_replication_queued", "file=%s op=CREATE primary=%s replica=%s", 
                         filename, selected_ss, replica);
            }
        }
        
        return send_success_response(client_fd, "", username, "File Created Successfully!");
    } else {
        Error err = error_simple(ERR_INTERNAL, "Failed to index file");
        return send_error_response(client_fd, "", username, &err);
    }
}

// Handle DELETE command
int handle_delete(int client_fd, const char *username, const char *filename) {
    if (!username || !filename || !client_fd) {
        Error err = error_simple(ERR_INVALID, "Invalid parameters");
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Lookup file in index
    FileEntry *entry = index_lookup_file(filename);
    if (!entry) {
        Error err = error_create(ERR_NOT_FOUND, "File '%s' not found", filename);
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Check if user is owner
    // Note: Full ACL check would require loading metadata from SS
    // For now, we check index owner field
    if (strcmp(entry->owner, username) != 0) {
        Error err = error_create(ERR_UNAUTHORIZED, "User '%s' is not the owner of file '%s'", username, filename);
        return send_error_response(client_fd, "", username, &err);
    }

    char full_path[ACL_CACHE_KEY_MAX];
    build_full_path(entry, full_path, sizeof(full_path));
    
    // Connect to SS
    int ss_fd = get_ss_connection_for_file(entry);
    if (ss_fd < 0) {
        Error err = error_simple(ERR_UNAVAILABLE, "Cannot connect to storage server");
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Send DELETE command to SS
    Message delete_cmd = {0};
    (void)snprintf(delete_cmd.type, sizeof(delete_cmd.type), "%s", "DELETE");
    (void)snprintf(delete_cmd.id, sizeof(delete_cmd.id), "%s", "1");
    (void)snprintf(delete_cmd.username, sizeof(delete_cmd.username), "%s", username);
    (void)snprintf(delete_cmd.role, sizeof(delete_cmd.role), "%s", "NM");
    (void)snprintf(delete_cmd.payload, sizeof(delete_cmd.payload), "%s", filename);
    
    char cmd_line[MAX_LINE];
    proto_format_line(&delete_cmd, cmd_line, sizeof(cmd_line));
    if (send_all(ss_fd, cmd_line, strlen(cmd_line)) != 0) {
        close(ss_fd);
        Error err = error_simple(ERR_INTERNAL, "Failed to send command to storage server");
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Wait for ACK from SS
    char resp_buf[MAX_LINE];
    int n = recv_line(ss_fd, resp_buf, sizeof(resp_buf));
    close(ss_fd);
    
    if (n <= 0) {
        Error err = error_simple(ERR_INTERNAL, "No response from storage server");
        return send_error_response(client_fd, "", username, &err);
    }
    
    Message ss_resp;
    if (proto_parse_line(resp_buf, &ss_resp) != 0) {
        Error err = error_simple(ERR_INTERNAL, "Invalid response from storage server");
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Check if SS returned error
    if (strcmp(ss_resp.type, "ERROR") == 0) {
        char error_code[64];
        char error_msg[256];
        proto_parse_error(&ss_resp, error_code, sizeof(error_code),
                         error_msg, sizeof(error_msg));
        Error err = error_simple(ERR_INTERNAL, error_msg);
        return send_error_response(client_fd, "", username, &err);
    }
    
    // SS deleted file successfully - remove from index
    if (index_remove_file(filename) == 0) {
        log_info("nm_file_deleted", "file=%s owner=%s", filename, username);
        registry_adjust_ss_file_count(entry->ss_username, -1);
        
        // Queue async replication deletion to backup SS
        const char *replica = replication_get_replica(entry->ss_username);
        if (replica) {
            if (replication_worker_queue(REPL_OP_DELETE, filename, entry->ss_username, replica) == 0) {
                log_info("nm_replication_queued", "file=%s op=DELETE primary=%s replica=%s", 
                         filename, entry->ss_username, replica);
            }
        }
        
        // Remove any pending access requests for this file
        request_queue_remove_by_filename(entry->filename, entry->folder_path);

        acl_cache_invalidate(full_path);
        
        return send_success_response(client_fd, "", username, "File deleted successfully!");
    } else {
        Error err = error_simple(ERR_INTERNAL, "Failed to remove file from index");
        return send_error_response(client_fd, "", username, &err);
    }
}

// Handle INFO command
int handle_info(int client_fd, const char *username, const char *filename) {
    if (!username || !filename || !client_fd) {
        Error err = error_simple(ERR_INVALID, "Invalid parameters");
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Lookup file in index
    FileEntry *entry = index_lookup_file(filename);
    if (!entry) {
        Error err = error_create(ERR_NOT_FOUND, "File '%s' not found", filename);
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Load owner from SS metadata if not already set
    if (entry->owner[0] == '\0') {
        load_owner_from_ss(entry);
    }
    
    // Note: Full ACL check would require loading metadata from SS
    // For now, we allow if user is owner (simplified)
    if (strcmp(entry->owner, username) != 0) {
        // Could check read access here via ACL, but for now just check owner
        // Future: Load ACL from SS metadata and check read access
    }
    
    // Update last accessed timestamp
    time_t now = time(NULL);
    index_update_metadata(filename, now, 0, entry->size_bytes,
                         entry->word_count, entry->char_count);
    
    // Format INFO output
    char output[1024] = {0};
    char created_str[32], modified_str[32], accessed_str[32];
    
    struct tm *tm = localtime(&entry->created);
    strftime(created_str, sizeof(created_str), "%Y-%m-%d %H:%M", tm);
    tm = localtime(&entry->last_modified);
    strftime(modified_str, sizeof(modified_str), "%Y-%m-%d %H:%M", tm);
    tm = localtime(&entry->last_accessed);
    strftime(accessed_str, sizeof(accessed_str), "%Y-%m-%d %H:%M", tm);
    
    (void)snprintf(output, sizeof(output),
                  "--> File: %s\n"
                  "--> Owner: %s\n"
                  "--> Created: %s\n"
                  "--> Last Modified: %s\n"
                  "--> Size: %zu bytes\n"
                  "--> Words: %d\n"
                  "--> Characters: %d\n"
                  "--> Last Accessed: %s by %s\n",
                  filename, entry->owner, created_str, modified_str,
                  entry->size_bytes, entry->word_count, entry->char_count,
                  accessed_str, username);
    
    return send_data_response(client_fd, "", username, output);
}

// Handle LIST command
int handle_list(int client_fd, const char *username) {
    // Get all registered users from registry
    char output[4096] = {0};
    size_t output_pos = 0;
    
    char clients[100][64];
    int count = registry_get_clients(clients, 100);
    
    for (int i = 0; i < count && output_pos < sizeof(output) - 100; i++) {
        int n = snprintf(output + output_pos, sizeof(output) - output_pos,
                         "--> %s\n", clients[i]);
        if (n > 0) output_pos += n;
    }
    
    return send_data_response(client_fd, "", username, output);
}

// Handle READ command - Returns SS connection info for direct client connection
int handle_read(int client_fd, const char *username, const char *filename) {
    if (!username || !filename || !client_fd) {
        Error err = error_simple(ERR_INVALID, "Invalid parameters");
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Lookup file in index
    log_info("nm_read_lookup", "file=%s user=%s", filename, username);
    FileEntry *entry = index_lookup_file(filename);
    if (!entry) {
        log_error("nm_read_not_found", "file=%s user=%s", filename, username);
        Error err = error_create(ERR_NOT_FOUND, "File '%s' not found", filename);
        return send_error_response(client_fd, "", username, &err);
    }
    
    log_info("nm_read_found", "file=%s folder=%s basename=%s", filename, entry->folder_path, entry->filename);

    // Get active SS (primary or replica if primary failed)
    const char *active_host;
    int active_port;
    const char *active_ss_name;
    get_active_ss_for_file(entry, &active_host, &active_port, &active_ss_name);

    // Load ACL from SS and check read access
    ACL acl = {0};
    if (fetch_acl_from_ss(entry, &acl) != 0) {
        Error err = error_simple(ERR_INTERNAL, "Failed to load ACL");
        return send_error_response(client_fd, "", username, &err);
    }
    Error access_err = check_file_access(filename, username, 0, &acl);
    if (!error_is_ok(&access_err)) {
        return send_error_response(client_fd, "", username, &access_err);
    }
    
    // Format SS connection info: host=IP,port=PORT
    char ss_info[256];
    (void)snprintf(ss_info, sizeof(ss_info), "host=%s,port=%d", active_host, active_port);
    
    // Send SS_INFO message to client
    Message resp = {0};
    (void)snprintf(resp.type, sizeof(resp.type), "%s", "SS_INFO");
    (void)snprintf(resp.id, sizeof(resp.id), "%s", "");
    (void)snprintf(resp.username, sizeof(resp.username), "%s", username ? username : "");
    (void)snprintf(resp.role, sizeof(resp.role), "%s", "NM");
    (void)snprintf(resp.payload, sizeof(resp.payload), "%s", ss_info);
    
    char resp_buf[MAX_LINE];
    if (proto_format_line(&resp, resp_buf, sizeof(resp_buf)) != 0) {
        log_error("nm_read_fmt", "failed to format SS_INFO response");
        Error err = error_simple(ERR_INTERNAL, "Failed to format response");
        return send_error_response(client_fd, "", username, &err);
    }
    
    log_info("nm_read_ss_info", "file=%s user=%s ss=%s:%d", filename, username, active_host, active_port);
    return send_all(client_fd, resp_buf, strlen(resp_buf));
}

// Handle STREAM command - Returns SS connection info for direct client connection
int handle_stream(int client_fd, const char *username, const char *filename) {
    if (!username || !filename || !client_fd) {
        Error err = error_simple(ERR_INVALID, "Invalid parameters");
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Lookup file in index
    FileEntry *entry = index_lookup_file(filename);
    if (!entry) {
        Error err = error_create(ERR_NOT_FOUND, "File '%s' not found", filename);
        return send_error_response(client_fd, "", username, &err);
    }

    // Get active SS (primary or replica if primary failed)
    const char *active_host;
    int active_port;
    const char *active_ss_name;
    get_active_ss_for_file(entry, &active_host, &active_port, &active_ss_name);

    // Load ACL and check read access
    ACL acl = {0};
    if (fetch_acl_from_ss(entry, &acl) != 0) {
        Error err = error_simple(ERR_INTERNAL, "Failed to load ACL");
        return send_error_response(client_fd, "", username, &err);
    }
    Error access_err = check_file_access(filename, username, 0, &acl);
    if (!error_is_ok(&access_err)) {
        return send_error_response(client_fd, "", username, &access_err);
    }
    
    // Format SS connection info: host=IP,port=PORT
    char ss_info[256];
    (void)snprintf(ss_info, sizeof(ss_info), "host=%s,port=%d", active_host, active_port);
    
    // Send SS_INFO message to client
    Message resp = {0};
    (void)snprintf(resp.type, sizeof(resp.type), "%s", "SS_INFO");
    (void)snprintf(resp.id, sizeof(resp.id), "%s", "");
    (void)snprintf(resp.username, sizeof(resp.username), "%s", username ? username : "");
    (void)snprintf(resp.role, sizeof(resp.role), "%s", "NM");
    (void)snprintf(resp.payload, sizeof(resp.payload), "%s", ss_info);
    
    char resp_buf[MAX_LINE];
    if (proto_format_line(&resp, resp_buf, sizeof(resp_buf)) != 0) {
        log_error("nm_stream_fmt", "failed to format SS_INFO response");
        Error err = error_simple(ERR_INTERNAL, "Failed to format response");
        return send_error_response(client_fd, "", username, &err);
    }
    
    log_info("nm_stream_ss_info", "file=%s user=%s ss=%s:%d", filename, username, active_host, active_port);
    return send_all(client_fd, resp_buf, strlen(resp_buf));
}

int handle_undo(int client_fd, const char *username, const char *filename) {
    if (!username || !filename || !client_fd) {
        Error err = error_simple(ERR_INVALID, "Invalid parameters");
        return send_error_response(client_fd, "", username, &err);
    }

    FileEntry *entry = index_lookup_file(filename);
    if (!entry) {
        Error err = error_create(ERR_NOT_FOUND, "File '%s' not found", filename);
        return send_error_response(client_fd, "", username, &err);
    }

    ACL acl = {0};
    if (fetch_acl_from_ss(entry, &acl) != 0) {
        Error err = error_simple(ERR_INTERNAL, "Failed to load ACL");
        return send_error_response(client_fd, "", username, &err);
    }
    Error access_err = check_file_access(filename, username, 0, &acl);
    if (!error_is_ok(&access_err)) {
        return send_error_response(client_fd, "", username, &access_err);
    }

    char ss_info[256];
    (void)snprintf(ss_info, sizeof(ss_info), "host=%s,port=%d", entry->ss_host, entry->ss_client_port);

    Message resp = {0};
    (void)snprintf(resp.type, sizeof(resp.type), "%s", "SS_INFO");
    (void)snprintf(resp.id, sizeof(resp.id), "%s", "");
    (void)snprintf(resp.username, sizeof(resp.username), "%s", username ? username : "");
    (void)snprintf(resp.role, sizeof(resp.role), "%s", "NM");
    (void)snprintf(resp.payload, sizeof(resp.payload), "%s", ss_info);

    char resp_buf[MAX_LINE];
    if (proto_format_line(&resp, resp_buf, sizeof(resp_buf)) != 0) {
        Error err = error_simple(ERR_INTERNAL, "Failed to format response");
        return send_error_response(client_fd, "", username, &err);
    }
    log_info("nm_cmd_undo", "user=%s file=%s", username, filename);
    return send_all(client_fd, resp_buf, strlen(resp_buf));
}

int handle_exec(int client_fd, const char *username, const char *filename, const char *request_id) {
    if (!username || !filename || !client_fd) {
        Error err = error_simple(ERR_INVALID, "Invalid parameters");
        return send_error_response(client_fd, "", username, &err);
    }

    FileEntry *entry = index_lookup_file(filename);
    if (!entry) {
        Error err = error_create(ERR_NOT_FOUND, "File '%s' not found", filename);
        return send_error_response(client_fd, "", username, &err);
    }

    ACL acl = {0};
    if (fetch_acl_from_ss(entry, &acl) != 0) {
        Error err = error_simple(ERR_INTERNAL, "Failed to load ACL");
        return send_error_response(client_fd, "", username, &err);
    }
    Error access_err = check_file_access(filename, username, 0, &acl);
    if (!error_is_ok(&access_err)) {
        return send_error_response(client_fd, "", username, &access_err);
    }

    char *script_text = NULL;
    if (fetch_file_content_from_ss(entry, &script_text) != 0) {
        Error err = error_simple(ERR_INTERNAL, "Failed to fetch file content");
        return send_error_response(client_fd, "", username, &err);
    }

    char *output_text = NULL;
    char exec_err[256];
    if (execute_script_text(script_text, &output_text, exec_err, sizeof(exec_err)) != 0) {
        free(script_text);
        Error err = error_simple(ERR_INTERNAL, exec_err);
        return send_error_response(client_fd, "", username, &err);
    }
    free(script_text);

    if (send_streaming_response(client_fd, request_id ? request_id : "", username, output_text) != 0) {
        free(output_text);
        Error err = error_simple(ERR_INTERNAL, "Failed to send EXEC output");
        return send_error_response(client_fd, "", username, &err);
    }
    free(output_text);
    return 0;
}

int handle_write(int client_fd, const char *username, const char *filename, int sentence_index) {
    (void)sentence_index;
    if (!username || !filename || !client_fd) {
        Error err = error_simple(ERR_INVALID, "Invalid parameters");
        return send_error_response(client_fd, "", username, &err);
    }

    FileEntry *entry = index_lookup_file(filename);
    if (!entry) {
        Error err = error_create(ERR_NOT_FOUND, "File '%s' not found", filename);
        return send_error_response(client_fd, "", username, &err);
    }

    // Get active SS (primary or replica if primary failed)
    const char *active_host;
    int active_port;
    const char *active_ss_name;
    get_active_ss_for_file(entry, &active_host, &active_port, &active_ss_name);

    ACL acl = {0};
    if (fetch_acl_from_ss(entry, &acl) != 0) {
        Error err = error_simple(ERR_INTERNAL, "Failed to load ACL");
        return send_error_response(client_fd, "", username, &err);
    }
    Error access_err = check_file_access(filename, username, 1, &acl);
    if (!error_is_ok(&access_err)) {
        return send_error_response(client_fd, "", username, &access_err);
    }

    char ss_info[256];
    (void)snprintf(ss_info, sizeof(ss_info), "host=%s,port=%d", active_host, active_port);

    Message resp = {0};
    (void)snprintf(resp.type, sizeof(resp.type), "%s", "SS_INFO");
    (void)snprintf(resp.id, sizeof(resp.id), "%s", "");
    (void)snprintf(resp.username, sizeof(resp.username), "%s", username ? username : "");
    (void)snprintf(resp.role, sizeof(resp.role), "%s", "NM");
    (void)snprintf(resp.payload, sizeof(resp.payload), "%s", ss_info);

    char resp_buf[MAX_LINE];
    if (proto_format_line(&resp, resp_buf, sizeof(resp_buf)) != 0) {
        Error err = error_simple(ERR_INTERNAL, "Failed to format response");
        return send_error_response(client_fd, "", username, &err);
    }
    log_info("nm_cmd_write", "file=%s user=%s", filename, username);
    return send_all(client_fd, resp_buf, strlen(resp_buf));
}

// Handle ADDACCESS command
int handle_addaccess(int client_fd, const char *username, const char *flag,
                     const char *filename, const char *target_username) {
    if (!username || !flag || !filename || !target_username || !client_fd) {
        Error err = error_simple(ERR_INVALID, "Invalid parameters");
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Lookup file in index
    FileEntry *entry = index_lookup_file(filename);
    if (!entry) {
        Error err = error_create(ERR_NOT_FOUND, "File '%s' not found", filename);
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Load ACL and verify requester is owner
    ACL acl = {0};
    if (fetch_acl_from_ss(entry, &acl) != 0) {
        Error err = error_simple(ERR_INTERNAL, "Failed to load ACL");
        return send_error_response(client_fd, "", username, &err);
    }
    Error owner_err = check_file_owner(filename, username, &acl);
    if (!error_is_ok(&owner_err)) {
        return send_error_response(client_fd, "", username, &owner_err);
    }
    
    // Connect to SS
    int ss_fd = get_ss_connection_for_file(entry);
    if (ss_fd < 0) {
        Error err = error_simple(ERR_UNAVAILABLE, "Cannot connect to storage server");
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Send UPDATE_ACL command to SS
    Message update_cmd = {0};
    (void)snprintf(update_cmd.type, sizeof(update_cmd.type), "%s", "UPDATE_ACL");
    (void)snprintf(update_cmd.id, sizeof(update_cmd.id), "%s", "1");
    (void)snprintf(update_cmd.username, sizeof(update_cmd.username), "%s", username);
    (void)snprintf(update_cmd.role, sizeof(update_cmd.role), "%s", "NM");
    
    // Payload: action=ADD,flag=R|W,filename=FILE,target_user=USER
    (void)snprintf(update_cmd.payload, sizeof(update_cmd.payload),
                   "action=ADD,flag=%s,filename=%s,target_user=%s", flag, filename, target_username);
    
    char cmd_line[MAX_LINE];
    proto_format_line(&update_cmd, cmd_line, sizeof(cmd_line));
    if (send_all(ss_fd, cmd_line, strlen(cmd_line)) != 0) {
        close(ss_fd);
        Error err = error_simple(ERR_INTERNAL, "Failed to send command to storage server");
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Wait for ACK from SS
    char resp_buf[MAX_LINE];
    int n = recv_line(ss_fd, resp_buf, sizeof(resp_buf));
    close(ss_fd);
    
    if (n <= 0) {
        Error err = error_simple(ERR_INTERNAL, "No response from storage server");
        return send_error_response(client_fd, "", username, &err);
    }
    
    Message ss_resp;
    if (proto_parse_line(resp_buf, &ss_resp) != 0) {
        Error err = error_simple(ERR_INTERNAL, "Invalid response from storage server");
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Check if SS returned error
    if (strcmp(ss_resp.type, "ERROR") == 0) {
        char error_code[64];
        char error_msg[256];
        proto_parse_error(&ss_resp, error_code, sizeof(error_code),
                         error_msg, sizeof(error_msg));
        Error err = error_simple(ERR_INTERNAL, error_msg);
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Success
    char full_path[ACL_CACHE_KEY_MAX];
    build_full_path(entry, full_path, sizeof(full_path));
    acl_cache_invalidate(full_path);

    log_info("nm_access_granted", "file=%s owner=%s target=%s flag=%s",
             filename, username, target_username, flag);
    return send_success_response(client_fd, "", username, "Access granted successfully!");
}

// Handle REMACCESS command
int handle_remaccess(int client_fd, const char *username,
                     const char *filename, const char *target_username) {
    if (!username || !filename || !target_username || !client_fd) {
        Error err = error_simple(ERR_INVALID, "Invalid parameters");
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Lookup file in index
    FileEntry *entry = index_lookup_file(filename);
    if (!entry) {
        Error err = error_create(ERR_NOT_FOUND, "File '%s' not found", filename);
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Load ACL and verify requester is owner
    ACL acl = {0};
    if (fetch_acl_from_ss(entry, &acl) != 0) {
        Error err = error_simple(ERR_INTERNAL, "Failed to load ACL");
        return send_error_response(client_fd, "", username, &err);
    }
    Error owner_err = check_file_owner(filename, username, &acl);
    if (!error_is_ok(&owner_err)) {
        return send_error_response(client_fd, "", username, &owner_err);
    }
    
    // Connect to SS
    int ss_fd = get_ss_connection_for_file(entry);
    if (ss_fd < 0) {
        Error err = error_simple(ERR_UNAVAILABLE, "Cannot connect to storage server");
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Send UPDATE_ACL command to SS
    Message update_cmd = {0};
    (void)snprintf(update_cmd.type, sizeof(update_cmd.type), "%s", "UPDATE_ACL");
    (void)snprintf(update_cmd.id, sizeof(update_cmd.id), "%s", "1");
    (void)snprintf(update_cmd.username, sizeof(update_cmd.username), "%s", username);
    (void)snprintf(update_cmd.role, sizeof(update_cmd.role), "%s", "NM");
    
    // Payload: action=REMOVE,flag=,filename=FILE,target_user=USER
    (void)snprintf(update_cmd.payload, sizeof(update_cmd.payload),
                   "action=REMOVE,flag=,filename=%s,target_user=%s", filename, target_username);
    
    char cmd_line[MAX_LINE];
    proto_format_line(&update_cmd, cmd_line, sizeof(cmd_line));
    if (send_all(ss_fd, cmd_line, strlen(cmd_line)) != 0) {
        close(ss_fd);
        Error err = error_simple(ERR_INTERNAL, "Failed to send command to storage server");
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Wait for ACK from SS
    char resp_buf[MAX_LINE];
    int n = recv_line(ss_fd, resp_buf, sizeof(resp_buf));
    close(ss_fd);
    
    if (n <= 0) {
        Error err = error_simple(ERR_INTERNAL, "No response from storage server");
        return send_error_response(client_fd, "", username, &err);
    }
    
    Message ss_resp;
    if (proto_parse_line(resp_buf, &ss_resp) != 0) {
        Error err = error_simple(ERR_INTERNAL, "Invalid response from storage server");
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Check if SS returned error
    if (strcmp(ss_resp.type, "ERROR") == 0) {
        char error_code[64];
        char error_msg[256];
        proto_parse_error(&ss_resp, error_code, sizeof(error_code),
                         error_msg, sizeof(error_msg));
        Error err = error_simple(ERR_INTERNAL, error_msg);
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Success
    char full_path[ACL_CACHE_KEY_MAX];
    build_full_path(entry, full_path, sizeof(full_path));
    acl_cache_invalidate(full_path);

    log_info("nm_access_removed", "file=%s owner=%s target=%s",
             filename, username, target_username);
    return send_success_response(client_fd, "", username, "Access removed successfully!");
}

// ===== Folder Command Handlers =====

// Handle CREATE_FOLDER command
int handle_createfolder(int client_fd, const char *username, const char *folder_path) {
    if (!username || !folder_path) {
        Error err = error_simple(ERR_INVALID, "Invalid folder path");
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Validate folder path
    if (folder_path[0] != '/') {
        Error err = error_simple(ERR_INVALID, "Folder path must start with /");
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Check if folder already exists
    if (index_folder_exists(folder_path)) {
        Error err = error_simple(ERR_CONFLICT, "Folder already exists");
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Select SS (use least loaded)
    const char *ss_username = registry_get_least_loaded_ss();
    if (!ss_username) {
        Error err = error_simple(ERR_UNAVAILABLE, "No storage servers available");
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Get SS connection info
    char ss_host[64] = {0};
    int ss_client_port = 0;
    if (registry_get_ss_info(ss_username, ss_host, sizeof(ss_host), &ss_client_port) != 0) {
        Error err = error_simple(ERR_INTERNAL, "Failed to get storage server info");
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Connect to SS and send CREATE_FOLDER command
    int ss_fd = connect_to_host(ss_host, ss_client_port);
    if (ss_fd < 0) {
        Error err = error_simple(ERR_UNAVAILABLE, "Failed to connect to storage server");
        return send_error_response(client_fd, "", username, &err);
    }
    
    Message req = {0};
    (void)snprintf(req.type, sizeof(req.type), "%s", "CREATE_FOLDER");
    (void)snprintf(req.id, sizeof(req.id), "%s", "1");
    (void)snprintf(req.username, sizeof(req.username), "%s", username);
    (void)snprintf(req.role, sizeof(req.role), "%s", "NM");
    (void)snprintf(req.payload, sizeof(req.payload), "%s", folder_path);
    
    char req_buf[MAX_LINE];
    if (proto_format_line(&req, req_buf, sizeof(req_buf)) != 0 ||
        send_all(ss_fd, req_buf, strlen(req_buf)) != 0) {
        close(ss_fd);
        Error err = error_simple(ERR_INTERNAL, "Failed to send command to storage server");
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Wait for response
    char resp_buf[MAX_LINE];
    int n = recv_line(ss_fd, resp_buf, sizeof(resp_buf));
    close(ss_fd);
    
    if (n <= 0) {
        Error err = error_simple(ERR_INTERNAL, "No response from storage server");
        return send_error_response(client_fd, "", username, &err);
    }
    
    Message ss_resp;
    if (proto_parse_line(resp_buf, &ss_resp) != 0) {
        Error err = error_simple(ERR_INTERNAL, "Invalid response from storage server");
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Check if SS returned error
    if (strcmp(ss_resp.type, "ERROR") == 0) {
        char error_code[64];
        char error_msg[256];
        proto_parse_error(&ss_resp, error_code, sizeof(error_code),
                         error_msg, sizeof(error_msg));
        Error err = error_simple(ERR_INTERNAL, error_msg);
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Add folder to index
    index_add_folder(folder_path, ss_username);
    
    log_info("nm_folder_created", "folder=%s user=%s", folder_path, username);
    return send_success_response(client_fd, "", username, "Folder created successfully!");
}

// Handle MOVE command
int handle_move(int client_fd, const char *username, const char *filename,
                const char *new_folder_path) {
    if (!username || !filename || !new_folder_path) {
        Error err = error_simple(ERR_INVALID, "Invalid parameters");
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Look up file
    FileEntry *entry = index_lookup_file(filename);
    if (!entry) {
        Error err = error_simple(ERR_NOT_FOUND, "File not found");
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Check if destination folder exists (except for root)
    if (strcmp(new_folder_path, "/") != 0 && !index_folder_exists(new_folder_path)) {
        Error err = error_simple(ERR_NOT_FOUND, "Destination folder does not exist");
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Check write access (fetch ACL from SS)
    ACL acl;
    if (fetch_acl_from_ss(entry, &acl) != 0) {
        Error err = error_simple(ERR_INTERNAL, "Failed to fetch file permissions");
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Check access
    if (!acl_check_write(&acl, username)) {
        Error err = error_simple(ERR_UNAUTHORIZED, "You do not have write permission for this file");
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Connect to SS and send MOVE command
    int ss_fd = connect_to_host(entry->ss_host, entry->ss_client_port);
    if (ss_fd < 0) {
        Error err = error_simple(ERR_UNAVAILABLE, "Failed to connect to storage server");
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Build payload: "filename|old_folder_path|new_folder_path"
    char payload[1024];
    snprintf(payload, sizeof(payload), "%s|%s|%s", 
             entry->filename, entry->folder_path, new_folder_path);
    
    Message req = {0};
    (void)snprintf(req.type, sizeof(req.type), "%s", "MOVE");
    (void)snprintf(req.id, sizeof(req.id), "%s", "1");
    (void)snprintf(req.username, sizeof(req.username), "%s", username);
    (void)snprintf(req.role, sizeof(req.role), "%s", "NM");
    (void)snprintf(req.payload, sizeof(req.payload), "%s", payload);
    
    char req_buf[MAX_LINE];
    if (proto_format_line(&req, req_buf, sizeof(req_buf)) != 0 ||
        send_all(ss_fd, req_buf, strlen(req_buf)) != 0) {
        close(ss_fd);
        Error err = error_simple(ERR_INTERNAL, "Failed to send command to storage server");
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Wait for response
    char resp_buf[MAX_LINE];
    int n = recv_line(ss_fd, resp_buf, sizeof(resp_buf));
    close(ss_fd);
    
    if (n <= 0) {
        Error err = error_simple(ERR_INTERNAL, "No response from storage server");
        return send_error_response(client_fd, "", username, &err);
    }
    
    Message ss_resp;
    if (proto_parse_line(resp_buf, &ss_resp) != 0) {
        Error err = error_simple(ERR_INTERNAL, "Invalid response from storage server");
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Check if SS returned error
    if (strcmp(ss_resp.type, "ERROR") == 0) {
        char error_code[64];
        char error_msg[256];
        proto_parse_error(&ss_resp, error_code, sizeof(error_code),
                         error_msg, sizeof(error_msg));
        Error err = error_simple(ERR_INTERNAL, error_msg);
        return send_error_response(client_fd, "", username, &err);
    }
    
    char old_path[ACL_CACHE_KEY_MAX];
    build_full_path(entry, old_path, sizeof(old_path));

    // Update index
    index_move_file(entry->filename, entry->folder_path, new_folder_path);
    
    // Update pending access requests with new folder path
    // Filename stays the same, only folder changes
    request_queue_update_filename(entry->filename, entry->folder_path,
                                   entry->filename, new_folder_path);
    
    acl_cache_invalidate(old_path);

    log_info("nm_file_moved", "file=%s user=%s from=%s to=%s",
             filename, username, entry->folder_path, new_folder_path);
    return send_success_response(client_fd, "", username, "File moved successfully!");
}

// Handle VIEWFOLDER command
int handle_viewfolder(int client_fd, const char *username, const char *folder_path) {
    if (!username || !folder_path) {
        Error err = error_simple(ERR_INVALID, "Invalid folder path");
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Check if folder exists
    if (!index_folder_exists(folder_path)) {
        Error err = error_simple(ERR_NOT_FOUND, "Folder not found");
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Get files in folder
    FileEntry *files[1000];
    int file_count = index_get_files_in_folder(folder_path, files, 1000);
    
    // Get subfolders in folder
    FolderEntry *folders[1000];
    int folder_count = index_get_subfolders(folder_path, folders, 1000);
    
    // Build response
    char response[8192] = {0};
    char *p = response;
    size_t remaining = sizeof(response);
    
    // Add header
    int written = snprintf(p, remaining, "Contents of %s:\n", folder_path);
    if (written > 0) { p += written; remaining -= written; }
    
    // List folders
    if (folder_count > 0) {
        written = snprintf(p, remaining, "\nFolders:\n");
        if (written > 0) { p += written; remaining -= written; }
        
        for (int i = 0; i < folder_count && remaining > 0; i++) {
            // Extract folder name from full path
            const char *folder_name = folders[i]->folder_path;
            const char *last_slash = strrchr(folder_name, '/');
            if (last_slash && last_slash != folder_name) {
                // Find second-to-last slash
                const char *prev = folder_name;
                while (prev < last_slash) {
                    const char *next_slash = strchr(prev + 1, '/');
                    if (next_slash >= last_slash) break;
                    prev = next_slash;
                }
                if (prev < last_slash) {
                    folder_name = prev + 1;
                }
            }
            
            written = snprintf(p, remaining, "  [DIR] %s\n", folder_name);
            if (written > 0) { p += written; remaining -= written; }
        }
    }
    
    // List files
    if (file_count > 0) {
        written = snprintf(p, remaining, "\nFiles:\n");
        if (written > 0) { p += written; remaining -= written; }
        
        for (int i = 0; i < file_count && remaining > 0; i++) {
            written = snprintf(p, remaining, "  %s\n", files[i]->filename);
            if (written > 0) { p += written; remaining -= written; }
        }
    }
    
    if (file_count == 0 && folder_count == 0) {
        written = snprintf(p, remaining, "\n(empty)\n");
        if (written > 0) { p += written; remaining -= written; }
    }
    
    log_info("nm_viewfolder", "folder=%s user=%s files=%d folders=%d", 
             folder_path, username, file_count, folder_count);
    return send_data_response(client_fd, "", username, response);
}

// Handle REQUESTACCESS command
// Payload format: "filename|access_type" (e.g., "/documents/report.txt|R")
int handle_requestaccess(int client_fd, const char *username, const char *payload) {
    if (!username || !payload) {
        Error err = error_simple(ERR_INVALID, "Invalid parameters");
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Parse payload: filename|access_type
    char filename[256] = {0};
    char access_type_str[8] = {0};
    
    const char *pipe = strchr(payload, '|');
    if (!pipe) {
        Error err = error_simple(ERR_INVALID, "Invalid payload format. Expected: filename|access_type");
        return send_error_response(client_fd, "", username, &err);
    }
    
    size_t filename_len = pipe - payload;
    if (filename_len >= sizeof(filename)) {
        Error err = error_simple(ERR_INVALID, "Filename too long");
        return send_error_response(client_fd, "", username, &err);
    }
    
    memcpy(filename, payload, filename_len);
    filename[filename_len] = '\0';
    
    strncpy(access_type_str, pipe + 1, sizeof(access_type_str) - 1);
    
    // Parse access type: R, W, or RW/B
    char access_type;
    if (strcmp(access_type_str, "R") == 0) {
        access_type = 'R';
    } else if (strcmp(access_type_str, "W") == 0) {
        access_type = 'W';
    } else if (strcmp(access_type_str, "RW") == 0 || strcmp(access_type_str, "B") == 0) {
        access_type = 'B';
    } else {
        Error err = error_simple(ERR_INVALID, "Invalid access type. Use R, W, or RW");
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Lookup file
    FileEntry *entry = index_lookup_file(filename);
    if (!entry) {
        Error err = error_create(ERR_NOT_FOUND, "File '%s' not found", filename);
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Check if user is the owner
    if (strcmp(entry->owner, username) == 0) {
        Error err = error_simple(ERR_INVALID, "Cannot request access to your own file");
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Check existing ACL from SS
    ACL acl = {0};
    if (fetch_acl_from_ss(entry, &acl) != 0) {
        Error err = error_simple(ERR_INTERNAL, "Failed to load file ACL");
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Check if user already has the requested access or more
    int has_read = acl_check_read(&acl, username);
    int has_write = acl_check_write(&acl, username);
    
    if (access_type == 'R' && has_read) {
        Error err = error_simple(ERR_INVALID, "You already have read access to this file");
        return send_error_response(client_fd, "", username, &err);
    }
    if (access_type == 'W' && has_write) {
        Error err = error_simple(ERR_INVALID, "You already have write access to this file");
        return send_error_response(client_fd, "", username, &err);
    }
    if (access_type == 'B' && has_read && has_write) {
        Error err = error_simple(ERR_INVALID, "You already have read/write access to this file");
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Add request to queue
    int request_id = request_queue_add(entry->filename, entry->folder_path, 
                                      username, entry->owner, access_type);
    
    if (request_id == -2) {
        Error err = error_simple(ERR_CONFLICT, "You already have a pending request for this file");
        return send_error_response(client_fd, "", username, &err);
    }
    if (request_id < 0) {
        Error err = error_simple(ERR_INTERNAL, "Failed to create access request");
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Persist request to SS metadata
    int ss_fd = get_ss_connection_for_file(entry);
    if (ss_fd >= 0) {
        char ss_payload[512];
        snprintf(ss_payload, sizeof(ss_payload), "%s|%d|%s|%c", 
                 filename, request_id, username, access_type);
        
        Message ss_req = {0};
        snprintf(ss_req.type, sizeof(ss_req.type), "ADD_REQUEST");
        snprintf(ss_req.id, sizeof(ss_req.id), "1");
        snprintf(ss_req.username, sizeof(ss_req.username), "%s", entry->owner);
        snprintf(ss_req.role, sizeof(ss_req.role), "NM");
        snprintf(ss_req.payload, sizeof(ss_req.payload), "%s", ss_payload);
        
        char req_buf[MAX_LINE];
        proto_format_line(&ss_req, req_buf, sizeof(req_buf));
        send_all(ss_fd, req_buf, strlen(req_buf));
        
        // Wait for ACK (don't fail if SS fails - request is in memory)
        char resp_buf[MAX_LINE];
        recv_line(ss_fd, resp_buf, sizeof(resp_buf));
        close(ss_fd);
        log_info("nm_request_persisted", "id=%d file=%s", request_id, filename);
    }
    
    char msg[256];
    snprintf(msg, sizeof(msg), "Access request created with ID #%d", request_id);
    log_info("nm_access_request_created", "id=%d file=%s user=%s owner=%s type=%c",
             request_id, filename, username, entry->owner, access_type);
    
    return send_success_response(client_fd, "", username, msg);
}

// Handle VIEWACCESSREQUESTS command
// Payload: optional filename (empty for all requests)
int handle_viewaccessrequests(int client_fd, const char *username, const char *payload) {
    if (!username) {
        Error err = error_simple(ERR_INVALID, "Invalid parameters");
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Parse optional filename filter
    char filename[256] = {0};
    char folder_path[512] = {0};
    const char *filter_filename = NULL;
    const char *filter_folder_path = NULL;
    
    if (payload && payload[0] != '\0') {
        strncpy(filename, payload, sizeof(filename) - 1);
        
        // Parse into folder_path and base filename
        const char *last_slash = strrchr(filename, '/');
        if (last_slash) {
            size_t folder_len = last_slash - filename + 1;
            if (folder_len < sizeof(folder_path)) {
                memcpy(folder_path, filename, folder_len);
                folder_path[folder_len] = '\0';
                filter_folder_path = folder_path;
            }
            strcpy(filename, last_slash + 1);
            filter_filename = filename;
        } else {
            strcpy(folder_path, "/");
            filter_folder_path = folder_path;
            filter_filename = filename;
        }
    }
    
    // Get requests for this user's files
    int count = 0;
    AccessRequest **requests = request_queue_get_by_owner_filtered(username, 
                                                                   filter_filename,
                                                                   filter_folder_path,
                                                                   &count);
    
    if (count == 0 || !requests) {
        const char *msg = payload && payload[0] ? "No pending requests for this file" : "No pending access requests";
        free(requests);
        return send_data_response(client_fd, "", username, msg);
    }
    
    // Build response
    char response[8192] = {0};
    char *p = response;
    size_t remaining = sizeof(response);
    
    int written = snprintf(p, remaining, "Pending Access Requests:\n");
    if (written > 0) { p += written; remaining -= written; }
    
    written = snprintf(p, remaining, 
                      "-----------------------------------------------------------------------\n"
                      "|   ID  | Requester    | File                     | Type | Date       |\n"
                      "|-------|--------------|--------------------------|------|------------|\n");
    if (written > 0) { p += written; remaining -= written; }
    
    for (int i = 0; i < count && remaining > 100; i++) {
        AccessRequest *req = requests[i];
        
        // Build full path
        char full_path[768];
        if (strcmp(req->folder_path, "/") == 0) {
            snprintf(full_path, sizeof(full_path), "%s", req->filename);
        } else {
            size_t folder_len = strlen(req->folder_path);
            if (folder_len > 0 && req->folder_path[folder_len - 1] == '/') {
                snprintf(full_path, sizeof(full_path), "%.*s/%s",
                        (int)(folder_len - 1), req->folder_path, req->filename);
            } else {
                snprintf(full_path, sizeof(full_path), "%s/%s", req->folder_path, req->filename);
            }
        }
        
        // Format access type
        const char *type_str = (req->access_type == 'R') ? "R" :
                              (req->access_type == 'W') ? "W" : "RW";
        
        // Format date
        char date_str[16];
        struct tm *tm = localtime(&req->requested_at);
        strftime(date_str, sizeof(date_str), "%Y-%m-%d", tm);
        
        // Truncate long paths
        char short_path[25];
        if (strlen(full_path) > 24) {
            snprintf(short_path, sizeof(short_path), "%.21s...", full_path);
        } else {
            snprintf(short_path, sizeof(short_path), "%s", full_path);
        }
        
        written = snprintf(p, remaining, "| #%-4d | %-12s | %-24s | %-4s | %-10s |\n",
                          req->request_id, req->requester, short_path, type_str, date_str);
        if (written > 0) { p += written; remaining -= written; }
    }
    
    written = snprintf(p, remaining, "-----------------------------------------------------------------------\n");
    if (written > 0) { p += written; remaining -= written; }
    
    free(requests);
    
    log_info("nm_viewaccessrequests", "user=%s count=%d", username, count);
    return send_data_response(client_fd, "", username, response);
}

// Handle APPROVEACCESSREQUEST command
// Payload: request_id
int handle_approveaccessrequest(int client_fd, const char *username, const char *payload) {
    if (!username || !payload) {
        Error err = error_simple(ERR_INVALID, "Invalid parameters");
        return send_error_response(client_fd, "", username, &err);
    }
    
    int request_id = atoi(payload);
    if (request_id <= 0) {
        Error err = error_simple(ERR_INVALID, "Invalid request ID");
        return send_error_response(client_fd, "", username, &err);
    }
    
    log_info("nm_approve_step1", "Looking up request_id=%d", request_id);
    
    // Get the request
    AccessRequest *req = request_queue_get_by_id(request_id);
    if (!req) {
        log_error("nm_approve_fail", "Request ID %d not found", request_id);
        Error err = error_simple(ERR_NOT_FOUND, "Request not found");
        return send_error_response(client_fd, "", username, &err);
    }
    
    log_info("nm_approve_step2", "Found request id=%d file=%s requester=%s owner=%s", 
             request_id, req->filename, req->requester, req->owner);
    
    // Verify user is the owner
    if (strcmp(req->owner, username) != 0) {
        log_error("nm_approve_fail", "User %s is not owner (owner is %s)", username, req->owner);
        Error err = error_simple(ERR_UNAUTHORIZED, "You are not the owner of this file");
        return send_error_response(client_fd, "", username, &err);
    }
    
    log_info("nm_approve_step3", "Building full path from folder=%s filename=%s", 
             req->folder_path, req->filename);
    
    // Build full path for file lookup
    char full_path[768];
    if (strcmp(req->folder_path, "/") == 0) {
        snprintf(full_path, sizeof(full_path), "%s", req->filename);
    } else {
        size_t folder_len = strlen(req->folder_path);
        if (folder_len > 0 && req->folder_path[folder_len - 1] == '/') {
            snprintf(full_path, sizeof(full_path), "%.*s/%s",
                     (int)(folder_len - 1), req->folder_path, req->filename);
        } else {
            snprintf(full_path, sizeof(full_path), "%s/%s", req->folder_path, req->filename);
        }
    }
    
    log_info("nm_approve_step4", "Looking up file with path: %s", full_path);
    
    // Lookup file to get SS info
    FileEntry *entry = index_lookup_file(full_path);
    if (!entry) {
        // File was deleted - remove request
        log_error("nm_approve_fail", "File %s not found in index", full_path);
        request_queue_remove(request_id);
        Error err = error_simple(ERR_NOT_FOUND, "File no longer exists");
        return send_error_response(client_fd, "", username, &err);
    }
    
    log_info("nm_approve_step5", "File found, connecting to SS %s:%d", 
             entry->ss_host, entry->ss_client_port);
    
    // Connect to SS and send ADDACCESS command
    int ss_fd = get_ss_connection_for_file(entry);
    if (ss_fd < 0) {
        log_error("nm_approve_fail", "Failed to connect to SS");
        Error err = error_simple(ERR_UNAVAILABLE, "Cannot connect to storage server");
        return send_error_response(client_fd, "", username, &err);
    }
    
    log_info("nm_approve_step6", "Connected to SS, building UPDATE_ACL payload");
    
    // Build UPDATE_ACL payload: action=ADD,flag=R|W,filename=FILE,target_user=USER
    char ss_payload[1024];
    const char *perm = (req->access_type == 'R') ? "R" :
                      (req->access_type == 'W') ? "W" : "B";
    snprintf(ss_payload, sizeof(ss_payload), "action=ADD,flag=%s,filename=%s,target_user=%s",
             perm, full_path, req->requester);
    
    log_info("nm_approve_step7", "Sending UPDATE_ACL to SS: payload=%s", ss_payload);
    
    Message ss_req = {0};
    snprintf(ss_req.type, sizeof(ss_req.type), "UPDATE_ACL");
    snprintf(ss_req.id, sizeof(ss_req.id), "1");
    snprintf(ss_req.username, sizeof(ss_req.username), "%s", username);
    snprintf(ss_req.role, sizeof(ss_req.role), "NM");
    snprintf(ss_req.payload, sizeof(ss_req.payload), "%s", ss_payload);
    
    char req_buf[MAX_LINE];
    proto_format_line(&ss_req, req_buf, sizeof(req_buf));
    
    if (send_all(ss_fd, req_buf, strlen(req_buf)) != 0) {
        close(ss_fd);
        log_error("nm_approve_fail", "Failed to send to SS");
        Error err = error_simple(ERR_INTERNAL, "Failed to send command to storage server");
        return send_error_response(client_fd, "", username, &err);
    }
    
    log_info("nm_approve_step8", "Waiting for SS response");
    
    // Wait for response
    char resp_buf[MAX_LINE];
    int n = recv_line(ss_fd, resp_buf, sizeof(resp_buf));
    close(ss_fd);
    
    log_info("nm_approve_step9", "Got SS response: n=%d", n);
    
    if (n <= 0) {
        log_error("nm_approve_fail", "No response from SS");
        Error err = error_simple(ERR_INTERNAL, "No response from storage server");
        return send_error_response(client_fd, "", username, &err);
    }
    
    log_info("nm_approve_step10", "Parsing SS response: %s", resp_buf);
    
    Message ss_resp;
    proto_parse_line(resp_buf, &ss_resp);
    
    if (strcmp(ss_resp.type, "ERROR") == 0) {
        char error_code[64], error_msg[256];
        proto_parse_error(&ss_resp, error_code, sizeof(error_code), error_msg, sizeof(error_msg));
        log_error("nm_approve_fail", "SS returned error: %s", error_msg);
        Error err = error_simple(ERR_INTERNAL, error_msg);
        return send_error_response(client_fd, "", username, &err);
    }
    
    log_info("nm_approve_step11", "SS success, removing request from queue and metadata");
    
    // Remove request from SS metadata
    int ss_fd2 = get_ss_connection_for_file(entry);
    if (ss_fd2 >= 0) {
        char rm_payload[1024];
        snprintf(rm_payload, sizeof(rm_payload), "%s|%d", full_path, request_id);
        
        Message rm_req = {0};
        snprintf(rm_req.type, sizeof(rm_req.type), "REMOVE_REQUEST");
        snprintf(rm_req.id, sizeof(rm_req.id), "1");
        snprintf(rm_req.username, sizeof(rm_req.username), "%s", username);
        snprintf(rm_req.role, sizeof(rm_req.role), "NM");
        snprintf(rm_req.payload, sizeof(rm_req.payload), "%s", rm_payload);
        
        char rm_buf[MAX_LINE];
        proto_format_line(&rm_req, rm_buf, sizeof(rm_buf));
        send_all(ss_fd2, rm_buf, strlen(rm_buf));
        
        char rm_resp[MAX_LINE];
        recv_line(ss_fd2, rm_resp, sizeof(rm_resp));
        close(ss_fd2);
    }
    
    // Success - remove request from queue
    request_queue_remove(request_id);

    acl_cache_invalidate(full_path);
    
    char msg[1024];
    snprintf(msg, sizeof(msg), "Access granted to %s for file %s", req->requester, full_path);
    log_info("nm_access_request_approved", "id=%d file=%s requester=%s owner=%s type=%c",
             request_id, full_path, req->requester, username, req->access_type);
    
    return send_success_response(client_fd, "", username, msg);
}

// Handle DISAPPROVEACCESSREQUEST command
// Payload: request_id
int handle_disapproveaccessrequest(int client_fd, const char *username, const char *payload) {
    if (!username || !payload) {
        Error err = error_simple(ERR_INVALID, "Invalid parameters");
        return send_error_response(client_fd, "", username, &err);
    }
    
    int request_id = atoi(payload);
    if (request_id <= 0) {
        Error err = error_simple(ERR_INVALID, "Invalid request ID");
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Get the request
    AccessRequest *req = request_queue_get_by_id(request_id);
    if (!req) {
        Error err = error_simple(ERR_NOT_FOUND, "Request not found");
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Verify user is the owner
    if (strcmp(req->owner, username) != 0) {
        Error err = error_simple(ERR_UNAUTHORIZED, "You are not the owner of this file");
        return send_error_response(client_fd, "", username, &err);
    }
    
    // Build full path to locate file
    char full_path[768];
    if (strcmp(req->folder_path, "/") == 0) {
        snprintf(full_path, sizeof(full_path), "%s", req->filename);
    } else {
        size_t folder_len = strlen(req->folder_path);
        if (folder_len > 0 && req->folder_path[folder_len - 1] == '/') {
            snprintf(full_path, sizeof(full_path), "%.*s/%s",
                     (int)(folder_len - 1), req->folder_path, req->filename);
        } else {
            snprintf(full_path, sizeof(full_path), "%s/%s", req->folder_path, req->filename);
        }
    }
    
    // Lookup file to get SS connection
    FileEntry *entry = index_lookup_file(full_path);
    
    // Remove request from SS metadata (if file still exists)
    if (entry) {
        int ss_fd = get_ss_connection_for_file(entry);
        if (ss_fd >= 0) {
            char rm_payload[1024];
            snprintf(rm_payload, sizeof(rm_payload), "%s|%d", full_path, request_id);
            
            Message rm_req = {0};
            snprintf(rm_req.type, sizeof(rm_req.type), "REMOVE_REQUEST");
            snprintf(rm_req.id, sizeof(rm_req.id), "1");
            snprintf(rm_req.username, sizeof(rm_req.username), "%s", username);
            snprintf(rm_req.role, sizeof(rm_req.role), "NM");
            snprintf(rm_req.payload, sizeof(rm_req.payload), "%s", rm_payload);
            
            char rm_buf[MAX_LINE];
            proto_format_line(&rm_req, rm_buf, sizeof(rm_buf));
            send_all(ss_fd, rm_buf, strlen(rm_buf));
            
            char rm_resp[MAX_LINE];
            recv_line(ss_fd, rm_resp, sizeof(rm_resp));
            close(ss_fd);
        }
    }
    
    // Remove request from queue
    request_queue_remove(request_id);
    
    log_info("nm_access_request_denied", "id=%d requester=%s owner=%s",
             request_id, req->requester, username);
    
    return send_success_response(client_fd, "", username, "Access request denied");
}

// ===== Checkpoint Handlers =====

int handle_checkpoint(int client_fd, const char *username, const char *filename, const char *tag) {
    if (!username || !filename || !tag || !client_fd) {
        Error err = error_simple(ERR_INVALID, "Invalid parameters");
        return send_error_response(client_fd, "", username, &err);
    }

    // Lookup file
    FileEntry *entry = index_lookup_file(filename);
    if (!entry) {
        Error err = error_create(ERR_NOT_FOUND, "File '%s' not found", filename);
        return send_error_response(client_fd, "", username, &err);
    }

    // Check write access (only write users can create checkpoints)
    ACL acl = {0};
    if (fetch_acl_from_ss(entry, &acl) != 0) {
        Error err = error_simple(ERR_INTERNAL, "Failed to load ACL");
        return send_error_response(client_fd, "", username, &err);
    }
    Error access_err = check_file_access(filename, username, 1, &acl);
    if (!error_is_ok(&access_err)) {
        return send_error_response(client_fd, "", username, &access_err);
    }

    // Connect to SS
    int ss_fd = get_ss_connection_for_file(entry);
    if (ss_fd < 0) {
        Error err = error_simple(ERR_INTERNAL, "Cannot connect to storage server");
        return send_error_response(client_fd, "", username, &err);
    }

    // Format request to SS: filename|tag
    char payload[512];
    snprintf(payload, sizeof(payload), "%s|%s", filename, tag);

    // Send CHECKPOINT command to SS
    Message ss_req = {0};
    snprintf(ss_req.type, sizeof(ss_req.type), "CHECKPOINT");
    snprintf(ss_req.id, sizeof(ss_req.id), "1");
    snprintf(ss_req.username, sizeof(ss_req.username), "%s", username);
    snprintf(ss_req.role, sizeof(ss_req.role), "NM");
    snprintf(ss_req.payload, sizeof(ss_req.payload), "%s", payload);

    char req_buf[MAX_LINE];
    proto_format_line(&ss_req, req_buf, sizeof(req_buf));
    if (send_all(ss_fd, req_buf, strlen(req_buf)) != 0) {
        close(ss_fd);
        Error err = error_simple(ERR_INTERNAL, "Failed to send request to SS");
        return send_error_response(client_fd, "", username, &err);
    }

    // Receive response from SS
    char ss_resp[MAX_LINE];
    if (recv_line(ss_fd, ss_resp, sizeof(ss_resp)) <= 0) {
        close(ss_fd);
        Error err = error_simple(ERR_INTERNAL, "No response from SS");
        return send_error_response(client_fd, "", username, &err);
    }
    close(ss_fd);

    // Parse SS response
    Message ss_msg = {0};
    if (proto_parse_line(ss_resp, &ss_msg) != 0) {
        Error err = error_simple(ERR_INTERNAL, "Invalid SS response");
        return send_error_response(client_fd, "", username, &err);
    }

    // Forward response to client
    log_info("nm_checkpoint", "user=%s file=%s tag=%s result=%s", 
             username, filename, tag, ss_msg.type);
    return send_all(client_fd, ss_resp, strlen(ss_resp));
}

int handle_viewcheckpoint(int client_fd, const char *username, const char *filename, const char *tag) {
    if (!username || !filename || !tag || !client_fd) {
        Error err = error_simple(ERR_INVALID, "Invalid parameters");
        return send_error_response(client_fd, "", username, &err);
    }

    // Lookup file
    FileEntry *entry = index_lookup_file(filename);
    if (!entry) {
        Error err = error_create(ERR_NOT_FOUND, "File '%s' not found", filename);
        return send_error_response(client_fd, "", username, &err);
    }

    // Check read access
    ACL acl = {0};
    if (fetch_acl_from_ss(entry, &acl) != 0) {
        Error err = error_simple(ERR_INTERNAL, "Failed to load ACL");
        return send_error_response(client_fd, "", username, &err);
    }
    Error access_err = check_file_access(filename, username, 0, &acl);
    if (!error_is_ok(&access_err)) {
        return send_error_response(client_fd, "", username, &access_err);
    }

    // Connect to SS
    int ss_fd = get_ss_connection_for_file(entry);
    if (ss_fd < 0) {
        Error err = error_simple(ERR_INTERNAL, "Cannot connect to storage server");
        return send_error_response(client_fd, "", username, &err);
    }

    // Format request to SS
    char payload[512];
    snprintf(payload, sizeof(payload), "%s|%s", filename, tag);

    // Send VIEWCHECKPOINT command to SS
    Message ss_req = {0};
    snprintf(ss_req.type, sizeof(ss_req.type), "VIEWCHECKPOINT");
    snprintf(ss_req.id, sizeof(ss_req.id), "1");
    snprintf(ss_req.username, sizeof(ss_req.username), "%s", username);
    snprintf(ss_req.role, sizeof(ss_req.role), "NM");
    snprintf(ss_req.payload, sizeof(ss_req.payload), "%s", payload);

    char req_buf[MAX_LINE];
    proto_format_line(&ss_req, req_buf, sizeof(req_buf));
    if (send_all(ss_fd, req_buf, strlen(req_buf)) != 0) {
        close(ss_fd);
        Error err = error_simple(ERR_INTERNAL, "Failed to send request to SS");
        return send_error_response(client_fd, "", username, &err);
    }

    // Relay all DATA messages from SS to client until STOP
    log_info("nm_viewcheckpoint", "user=%s file=%s tag=%s", username, filename, tag);
    while (1) {
        char ss_resp[MAX_LINE];
        int n = recv_line(ss_fd, ss_resp, sizeof(ss_resp));
        if (n <= 0) {
            close(ss_fd);
            Error err = error_simple(ERR_INTERNAL, "Connection to SS closed unexpectedly");
            return send_error_response(client_fd, "", username, &err);
        }
        
        // Forward message to client
        if (send_all(client_fd, ss_resp, strlen(ss_resp)) != 0) {
            close(ss_fd);
            return -1;
        }
        
        // Check if this is a STOP message
        Message msg;
        if (proto_parse_line(ss_resp, &msg) == 0) {
            if (strcmp(msg.type, "STOP") == 0) {
                break;
            }
            // If ERROR, also stop
            if (strcmp(msg.type, "ERROR") == 0) {
                break;
            }
        }
    }
    close(ss_fd);
    return 0;
}

int handle_revert_checkpoint(int client_fd, const char *username, const char *filename, const char *tag) {
    if (!username || !filename || !tag || !client_fd) {
        Error err = error_simple(ERR_INVALID, "Invalid parameters");
        return send_error_response(client_fd, "", username, &err);
    }

    // Lookup file
    FileEntry *entry = index_lookup_file(filename);
    if (!entry) {
        Error err = error_create(ERR_NOT_FOUND, "File '%s' not found", filename);
        return send_error_response(client_fd, "", username, &err);
    }

    // Check write access (only write users can revert)
    ACL acl = {0};
    if (fetch_acl_from_ss(entry, &acl) != 0) {
        Error err = error_simple(ERR_INTERNAL, "Failed to load ACL");
        return send_error_response(client_fd, "", username, &err);
    }
    Error access_err = check_file_access(filename, username, 1, &acl);
    if (!error_is_ok(&access_err)) {
        return send_error_response(client_fd, "", username, &access_err);
    }

    // Connect to SS
    int ss_fd = get_ss_connection_for_file(entry);
    if (ss_fd < 0) {
        Error err = error_simple(ERR_INTERNAL, "Cannot connect to storage server");
        return send_error_response(client_fd, "", username, &err);
    }

    // Format request to SS
    char payload[512];
    snprintf(payload, sizeof(payload), "%s|%s", filename, tag);

    // Send REVERT command to SS
    Message ss_req = {0};
    snprintf(ss_req.type, sizeof(ss_req.type), "REVERT");
    snprintf(ss_req.id, sizeof(ss_req.id), "1");
    snprintf(ss_req.username, sizeof(ss_req.username), "%s", username);
    snprintf(ss_req.role, sizeof(ss_req.role), "NM");
    snprintf(ss_req.payload, sizeof(ss_req.payload), "%s", payload);

    char req_buf[MAX_LINE];
    proto_format_line(&ss_req, req_buf, sizeof(req_buf));
    if (send_all(ss_fd, req_buf, strlen(req_buf)) != 0) {
        close(ss_fd);
        Error err = error_simple(ERR_INTERNAL, "Failed to send request to SS");
        return send_error_response(client_fd, "", username, &err);
    }

    // Receive response from SS
    char ss_resp[MAX_LINE];
    if (recv_line(ss_fd, ss_resp, sizeof(ss_resp)) <= 0) {
        close(ss_fd);
        Error err = error_simple(ERR_INTERNAL, "No response from SS");
        return send_error_response(client_fd, "", username, &err);
    }
    close(ss_fd);

    // Forward response to client
    log_info("nm_revert", "user=%s file=%s tag=%s", username, filename, tag);
    return send_all(client_fd, ss_resp, strlen(ss_resp));
}

int handle_listcheckpoints(int client_fd, const char *username, const char *filename) {
    if (!username || !filename || !client_fd) {
        Error err = error_simple(ERR_INVALID, "Invalid parameters");
        return send_error_response(client_fd, "", username, &err);
    }

    // Lookup file
    FileEntry *entry = index_lookup_file(filename);
    if (!entry) {
        Error err = error_create(ERR_NOT_FOUND, "File '%s' not found", filename);
        return send_error_response(client_fd, "", username, &err);
    }

    // Check read access
    ACL acl = {0};
    if (fetch_acl_from_ss(entry, &acl) != 0) {
        Error err = error_simple(ERR_INTERNAL, "Failed to load ACL");
        return send_error_response(client_fd, "", username, &err);
    }
    Error access_err = check_file_access(filename, username, 0, &acl);
    if (!error_is_ok(&access_err)) {
        return send_error_response(client_fd, "", username, &access_err);
    }

    // Connect to SS
    int ss_fd = get_ss_connection_for_file(entry);
    if (ss_fd < 0) {
        Error err = error_simple(ERR_INTERNAL, "Cannot connect to storage server");
        return send_error_response(client_fd, "", username, &err);
    }

    // Send LISTCHECKPOINTS command to SS
    Message ss_req = {0};
    snprintf(ss_req.type, sizeof(ss_req.type), "LISTCHECKPOINTS");
    snprintf(ss_req.id, sizeof(ss_req.id), "1");
    snprintf(ss_req.username, sizeof(ss_req.username), "%s", username);
    snprintf(ss_req.role, sizeof(ss_req.role), "NM");
    snprintf(ss_req.payload, sizeof(ss_req.payload), "%s", filename);

    char req_buf[MAX_LINE];
    proto_format_line(&ss_req, req_buf, sizeof(req_buf));
    if (send_all(ss_fd, req_buf, strlen(req_buf)) != 0) {
        close(ss_fd);
        Error err = error_simple(ERR_INTERNAL, "Failed to send request to SS");
        return send_error_response(client_fd, "", username, &err);
    }

    // Receive response from SS
    char ss_resp[MAX_LINE];
    if (recv_line(ss_fd, ss_resp, sizeof(ss_resp)) <= 0) {
        close(ss_fd);
        Error err = error_simple(ERR_INTERNAL, "No response from SS");
        return send_error_response(client_fd, "", username, &err);
    }
    close(ss_fd);

    // Forward response to client
    log_info("nm_listcheckpoints", "user=%s file=%s", username, filename);
    return send_all(client_fd, ss_resp, strlen(ss_resp));
}

