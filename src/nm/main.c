// Name Server (NM): accepts connections from SS/Clients and handles
// registration and heartbeats in Phase 1. Thread-per-connection model.
#define _POSIX_C_SOURCE 200809L  // For strdup
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../common/net.h"
#include "../common/log.h"
#include "../common/protocol.h"
#include "index.h"
#include "access_control.h"
#include "commands.h"
#include "registry.h"
#include "access_requests.h"
#include "heartbeat_monitor.h"
#include "replication.h"
#include "replication_worker.h"

// Argument passed to each connection handler thread.
typedef struct ClientConnArg {
    int fd;
    struct sockaddr_in addr;
} ClientConnArg;

static volatile int g_running = 1;

// Failover callback: called when SS fails
static void on_ss_failure(const char *ss_username) {
    log_error("failover_start", "SS %s failed, initiating failover", ss_username);
    
    // Trigger failover in replication system
    replication_failover(ss_username);
    
    // Get the replica for this failed SS
    const char *replica_ss = replication_get_replica(ss_username);
    if (!replica_ss) {
        log_warning("failover_no_replica", "No replica found for %s, cannot failover", ss_username);
        return;
    }
    
    log_info("failover_replica_found", "Found replica %s for failed %s", replica_ss, ss_username);
    
    // Get replica SS connection info
    char replica_host[64];
    int replica_port;
    if (registry_get_ss_info(replica_ss, replica_host, sizeof(replica_host), &replica_port) != 0) {
        log_error("failover_replica_info", "Failed to get info for replica %s", replica_ss);
        return;
    }
    
    log_info("failover_replica_info", "Replica %s at %s:%d", replica_ss, replica_host, replica_port);
    
    // Update file index: switch all files from failed SS to replica
    FileEntry *files[1024];
    int file_count = index_get_all_files(files, 1024);
    int updated = 0;
    
    for (int i = 0; i < file_count; i++) {
        FileEntry *entry = files[i];
        if (strcmp(entry->ss_username, ss_username) == 0) {
            // This file was on the failed primary, switch to replica
            log_info("failover_update_file", "Switching file %s from %s to %s", 
                     entry->filename, ss_username, replica_ss);
            
            // Update primary SS info to point to replica
            strncpy(entry->ss_host, replica_host, sizeof(entry->ss_host) - 1);
            entry->ss_host[sizeof(entry->ss_host) - 1] = '\0';
            entry->ss_client_port = replica_port;
            strncpy(entry->ss_username, replica_ss, sizeof(entry->ss_username) - 1);
            entry->ss_username[sizeof(entry->ss_username) - 1] = '\0';
            updated++;
        }
    }
    
    log_info("failover_complete", "Failover complete for %s: updated %d files to use replica %s", 
             ss_username, updated, replica_ss);
}

// Handle a single parsed message from a peer.
static void handle_message(int fd, const struct sockaddr_in *peer, const Message *msg) {
    char ip[INET_ADDRSTRLEN]; inet_ntop(AF_INET, &peer->sin_addr, ip, sizeof(ip));
    if (strcmp(msg->type, "SS_REGISTER") == 0) {
        // Step 3: Parse SS registration payload and index files
        // Payload format: "host=IP,client_port=PORT,storage=DIR,files=file1.txt,file2.txt,..."
        
        // Extract SS information from payload
        char ss_host[64] = {0};
        int ss_client_port = 0;
        char *host_start = strstr(msg->payload, "host=");
        char *port_start = strstr(msg->payload, "client_port=");
        
        if (host_start) {
            char *host_end = strchr(host_start + 5, ',');
            if (host_end) {
                size_t host_len = host_end - (host_start + 5);
                if (host_len < sizeof(ss_host)) {
                    memcpy(ss_host, host_start + 5, host_len);
                    ss_host[host_len] = '\0';
                }
            }
        }
        
        if (port_start) {
            ss_client_port = atoi(port_start + 12);
        }
        
        // Extract and parse file list
        char *files_start = strstr(msg->payload, "files=");
        int file_count = 0;
        
        if (files_start) {
            files_start += 6;  // Skip "files="
            if (*files_start != '\0') {
                // Parse comma-separated file list and index each file
                char *file_list = strdup(files_start);  // Make copy for parsing
                if (file_list) {
                    char *saveptr = NULL;
                    char *entry_str = strtok_r(file_list, ",", &saveptr);
                    
                    while (entry_str) {
                        char filename_buf[256] = {0};
                        char owner_buf[64] = {0};
                        size_t size_bytes = 0;
                        int words = 0;
                        int chars = 0;

                        char *field_ptr = NULL;
                        char *field = strtok_r(entry_str, "|", &field_ptr);
                        if (field) {
                            strncpy(filename_buf, field, sizeof(filename_buf) - 1);
                        }
                        field = strtok_r(NULL, "|", &field_ptr);
                        if (field && *field) {
                            strncpy(owner_buf, field, sizeof(owner_buf) - 1);
                        }
                        field = strtok_r(NULL, "|", &field_ptr);
                        if (field) {
                            size_bytes = (size_t)strtoull(field, NULL, 10);
                        }
                        field = strtok_r(NULL, "|", &field_ptr);
                        if (field) {
                            words = atoi(field);
                        }
                        field = strtok_r(NULL, "|", &field_ptr);
                        if (field) {
                            chars = atoi(field);
                        }

                        const char *final_owner = (owner_buf[0] != '\0') ? owner_buf : msg->username;

                        FileEntry *entry = index_add_file(filename_buf, final_owner, ss_host, 
                                                          ss_client_port, msg->username);
                        if (entry) {
                            entry->size_bytes = size_bytes;
                            entry->word_count = words;
                            entry->char_count = chars;
                            file_count++;
                            
                            // Auto-register folder if file has a folder path
                            if (strcmp(entry->folder_path, "/") != 0) {
                                index_add_folder(entry->folder_path, msg->username);
                            }
                            
                            log_info("nm_file_indexed", "file=%s ss=%s owner=%s", 
                                     filename_buf, msg->username, entry->owner);
                        }
                        entry_str = strtok_r(NULL, ",", &saveptr);
                    }
                    
                    free(file_list);
                }
            }
        }
        
        (void)registry_add("SS", msg->username, msg->payload);
        registry_set_ss_file_count(msg->username, file_count);
        
        // Check if this SS was previously failed (recovery scenario)
        SSStatus prev_status = heartbeat_monitor_get_status(msg->username);
        int is_recovery = (prev_status == SS_STATUS_FAILED);
        
        // Register SS for heartbeat monitoring
        heartbeat_monitor_register_ss(msg->username);
        
        // Assign replica for replication (ss1 → ss1_backup)
        // Only try to pair primary servers (not backup servers themselves)
        size_t username_len = strlen(msg->username);
        int is_backup = (username_len >= 7 && strcmp(msg->username + username_len - 7, "_backup") == 0);
        if (!is_backup) {
            replication_assign_replica(msg->username);
        }
        
        // If this is a recovery, trigger full sync
        if (is_recovery) {
            log_info("nm_ss_recovery", "SS %s recovered from failure, triggering full sync", msg->username);
            
            // Get the paired SS (source for sync) BEFORE calling recover (which changes status)
            const char *pair_ss = NULL;
            ReplicationPairStatus pair_status = replication_get_pair_status(msg->username);
            
            if (pair_status == REPL_STATUS_PRIMARY_FAILED) {
                // Primary recovered, sync from replica
                pair_ss = replication_get_replica(msg->username);
                log_info("nm_recovery_sync_source", "Primary %s will sync from replica %s", 
                         msg->username, pair_ss ? pair_ss : "none");
            } else {
                // Replica recovered, sync from primary
                const char *primary = replication_get_primary_for_replica(msg->username);
                if (primary) {
                    pair_ss = primary;
                    log_info("nm_recovery_sync_source", "Replica %s will sync from primary %s", 
                             msg->username, primary);
                }
            }
            
            // Now update the replication status
            replication_recover(msg->username);
            
            // Queue sync jobs for all files that should be on this SS
            if (pair_ss) {
                FileEntry *all_files[1024];
                int total_files = index_get_all_files(all_files, 1024);
                int sync_count = 0;
                
                // Queue sync for all files that were on the recovered SS
                for (int i = 0; i < total_files; i++) {
                    FileEntry *entry = all_files[i];
                    // Sync files that belong to this recovered SS (they may point to either SS now)
                    if (strcmp(entry->ss_username, pair_ss) == 0 ||
                        strcmp(entry->ss_username, msg->username) == 0) {
                        log_info("nm_recovery_sync_file", "Queueing %s from %s to %s",
                                 entry->filename, pair_ss, msg->username);
                        replication_worker_queue(REPL_OP_UPDATE, entry->filename, 
                                                pair_ss, msg->username);
                        sync_count++;
                    }
                }
                log_info("nm_recovery_sync_queued", "Queued %d files for recovery sync from %s to %s",
                         sync_count, pair_ss, msg->username);
            } else {
                log_warning("nm_recovery_no_pair", "No pair found for %s, cannot sync", msg->username);
            }
        }
        
        log_info("nm_ss_register", "ip=%s user=%s files=%d indexed", ip, msg->username, file_count);
        
        Message ack = {0};
        (void)snprintf(ack.type, sizeof(ack.type), "%s", "ACK");
        (void)snprintf(ack.id, sizeof(ack.id), "%s", msg->id);
        (void)snprintf(ack.username, sizeof(ack.username), "%s", msg->username);
        (void)snprintf(ack.role, sizeof(ack.role), "%s", "NM");
        (void)snprintf(ack.payload, sizeof(ack.payload), "%s", "registered");
        char line[MAX_LINE]; proto_format_line(&ack, line, sizeof(line));
        send_all(fd, line, strlen(line));
        return;
    }
    if (strcmp(msg->type, "CLIENT_REGISTER") == 0) {
        // printf("DEBUG: Received message type=CLIENT_REGISTER from %s\n", ip);
        (void)registry_add("CLIENT", msg->username, msg->payload);
        log_info("nm_client_register", "ip=%s user=%s", ip, msg->username);
        Message ack = {0};
        (void)snprintf(ack.type, sizeof(ack.type), "%s", "ACK");
        (void)snprintf(ack.id, sizeof(ack.id), "%s", msg->id);
        (void)snprintf(ack.username, sizeof(ack.username), "%s", msg->username);
        (void)snprintf(ack.role, sizeof(ack.role), "%s", "NM");
        (void)snprintf(ack.payload, sizeof(ack.payload), "%s", "registered");
        char line[MAX_LINE]; proto_format_line(&ack, line, sizeof(line));
        send_all(fd, line, strlen(line));
        return;
    }
    if (strcmp(msg->type, "HEARTBEAT") == 0) {
        // Update heartbeat timestamp for this SS
        heartbeat_monitor_update(msg->username);
        
        log_info("nm_heartbeat", "user=%s", msg->username);
        Message ack = {0};
        (void)snprintf(ack.type, sizeof(ack.type), "%s", "ACK");
        (void)snprintf(ack.id, sizeof(ack.id), "%s", msg->id);
        (void)snprintf(ack.username, sizeof(ack.username), "%s", msg->username);
        (void)snprintf(ack.role, sizeof(ack.role), "%s", "NM");
        (void)snprintf(ack.payload, sizeof(ack.payload), "%s", "pong");
        char line[MAX_LINE]; proto_format_line(&ack, line, sizeof(line));
        send_all(fd, line, strlen(line));
        return;
    }
    if (strcmp(msg->type, "WRITE_COMPLETE") == 0) {
        // SS notifies NM that file was written - queue replication
        const char *filename = msg->payload;
        const char *ss_username = msg->username;
        
        log_info("nm_write_complete", "file=%s ss=%s", filename, ss_username);
        
        // Queue replication to backup
        const char *replica = replication_get_replica(ss_username);
        if (replica) {
            if (replication_worker_queue(REPL_OP_UPDATE, filename, ss_username, replica) == 0) {
                log_info("nm_write_replication_queued", "file=%s primary=%s replica=%s", 
                         filename, ss_username, replica);
            }
        }
        
        // Send ACK
        Message ack = {0};
        (void)snprintf(ack.type, sizeof(ack.type), "%s", "ACK");
        (void)snprintf(ack.id, sizeof(ack.id), "%s", msg->id);
        (void)snprintf(ack.username, sizeof(ack.username), "%s", msg->username);
        (void)snprintf(ack.role, sizeof(ack.role), "%s", "NM");
        (void)snprintf(ack.payload, sizeof(ack.payload), "%s", "write_replication_queued");
        char line[MAX_LINE]; proto_format_line(&ack, line, sizeof(line));
        send_all(fd, line, strlen(line));
        return;
    }
    
    // Step 6: Handle client commands
    // Parse payload: flags=FLAGS|arg1|arg2|...
    if (strcmp(msg->type, "VIEW") == 0) {
        // printf("DEBUG: reached here 0 :%d", strcmp(msg->type, "VIEW"));
        
        // Extract flags from payload
        char flags[16] = {0};
        char *flags_start = strstr(msg->payload, "flags=");
        if (flags_start) {
            char *flags_end = strchr(flags_start + 6, '|');
            if (flags_end) {
                size_t flags_len = flags_end - (flags_start + 6);
                if (flags_len < sizeof(flags)) {
                    memcpy(flags, flags_start + 6, flags_len);
                    flags[flags_len] = '\0';
                }
            } else {
                strncpy(flags, flags_start + 6, sizeof(flags) - 1);
            }
        }
        
        log_info("nm_cmd_view", "user=%s flags=%s", msg->username, flags);
        // printf("DEBUG: reached here 1 :%d", strcmp(flags, "a"));
        handle_view(fd, msg->username, flags);
        return;
    }
    
    if (strcmp(msg->type, "CREATE") == 0) {
        // Filename is directly in payload
        char filename[256] = {0};
        size_t payload_len = strlen(msg->payload);
        size_t copy_len = (payload_len < sizeof(filename) - 1) ? payload_len : sizeof(filename) - 1;
        memcpy(filename, msg->payload, copy_len);
        filename[copy_len] = '\0';
        
        log_info("nm_cmd_create", "user=%s file=%s", msg->username, filename);
        handle_create(fd, msg->username, filename);
        return;
    }
    
    if (strcmp(msg->type, "DELETE") == 0) {
        // Filename is directly in payload
        char filename[256] = {0};
        size_t payload_len = strlen(msg->payload);
        size_t copy_len = (payload_len < sizeof(filename) - 1) ? payload_len : sizeof(filename) - 1;
        memcpy(filename, msg->payload, copy_len);
        filename[copy_len] = '\0';
        
        log_info("nm_cmd_delete", "user=%s file=%s", msg->username, filename);
        handle_delete(fd, msg->username, filename);
        return;
    }
    
    if (strcmp(msg->type, "INFO") == 0) {
        // Filename is directly in payload
        char filename[256] = {0};
        size_t payload_len = strlen(msg->payload);
        size_t copy_len = (payload_len < sizeof(filename) - 1) ? payload_len : sizeof(filename) - 1;
        memcpy(filename, msg->payload, copy_len);
        filename[copy_len] = '\0';
        
        log_info("nm_cmd_info", "user=%s file=%s", msg->username, filename);
        handle_info(fd, msg->username, filename);
        return;
    }
    
    if (strcmp(msg->type, "LIST") == 0) {
        log_info("nm_cmd_list", "user=%s", msg->username);
        handle_list(fd, msg->username);
        return;
    }
    
    if (strcmp(msg->type, "READ") == 0) {
        // Filename is directly in payload
        char filename[256] = {0};
        size_t payload_len = strlen(msg->payload);
        size_t copy_len = (payload_len < sizeof(filename) - 1) ? payload_len : sizeof(filename) - 1;
        memcpy(filename, msg->payload, copy_len);
        filename[copy_len] = '\0';
        
        log_info("nm_cmd_read", "user=%s file=%s", msg->username, filename);
        handle_read(fd, msg->username, filename);
        return;
    }
    
    if (strcmp(msg->type, "STREAM") == 0) {
        // Filename is directly in payload
        char filename[256] = {0};
        size_t payload_len = strlen(msg->payload);
        size_t copy_len = (payload_len < sizeof(filename) - 1) ? payload_len : sizeof(filename) - 1;
        memcpy(filename, msg->payload, copy_len);
        filename[copy_len] = '\0';
        
        log_info("nm_cmd_stream", "user=%s file=%s", msg->username, filename);
        handle_stream(fd, msg->username, filename);
        return;
    }

    if (strcmp(msg->type, "UNDO") == 0) {
        char filename[256] = {0};
        size_t payload_len = strlen(msg->payload);
        size_t copy_len = (payload_len < sizeof(filename) - 1) ? payload_len : sizeof(filename) - 1;
        memcpy(filename, msg->payload, copy_len);
        filename[copy_len] = '\0';

        log_info("nm_cmd_undo", "user=%s file=%s", msg->username, filename);
        handle_undo(fd, msg->username, filename);
        return;
    }

    if (strcmp(msg->type, "CHECKPOINT") == 0) {
        // Payload format: "filename|tag"
        char filename[256] = {0};
        char tag[64] = {0};
        char *sep = strchr(msg->payload, '|');
        if (sep) {
            size_t filename_len = sep - msg->payload;
            if (filename_len >= sizeof(filename)) filename_len = sizeof(filename) - 1;
            memcpy(filename, msg->payload, filename_len);
            filename[filename_len] = '\0';
            
            strncpy(tag, sep + 1, sizeof(tag) - 1);
            tag[sizeof(tag) - 1] = '\0';
            
            log_info("nm_cmd_checkpoint", "user=%s file=%s tag=%s", msg->username, filename, tag);
            handle_checkpoint(fd, msg->username, filename, tag);
        }
        return;
    }

    if (strcmp(msg->type, "VIEWCHECKPOINT") == 0) {
        // Payload format: "filename|tag"
        char filename[256] = {0};
        char tag[64] = {0};
        char *sep = strchr(msg->payload, '|');
        if (sep) {
            size_t filename_len = sep - msg->payload;
            if (filename_len >= sizeof(filename)) filename_len = sizeof(filename) - 1;
            memcpy(filename, msg->payload, filename_len);
            filename[filename_len] = '\0';
            
            strncpy(tag, sep + 1, sizeof(tag) - 1);
            tag[sizeof(tag) - 1] = '\0';
            
            log_info("nm_cmd_viewcheckpoint", "user=%s file=%s tag=%s", msg->username, filename, tag);
            handle_viewcheckpoint(fd, msg->username, filename, tag);
        }
        return;
    }

    if (strcmp(msg->type, "REVERT") == 0) {
        // Payload format: "filename|tag"
        char filename[256] = {0};
        char tag[64] = {0};
        char *sep = strchr(msg->payload, '|');
        if (sep) {
            size_t filename_len = sep - msg->payload;
            if (filename_len >= sizeof(filename)) filename_len = sizeof(filename) - 1;
            memcpy(filename, msg->payload, filename_len);
            filename[filename_len] = '\0';
            
            strncpy(tag, sep + 1, sizeof(tag) - 1);
            tag[sizeof(tag) - 1] = '\0';
            
            log_info("nm_cmd_revert", "user=%s file=%s tag=%s", msg->username, filename, tag);
            handle_revert_checkpoint(fd, msg->username, filename, tag);
        }
        return;
    }

    if (strcmp(msg->type, "LISTCHECKPOINTS") == 0) {
        char filename[256] = {0};
        size_t payload_len = strlen(msg->payload);
        size_t copy_len = (payload_len < sizeof(filename) - 1) ? payload_len : sizeof(filename) - 1;
        memcpy(filename, msg->payload, copy_len);
        filename[copy_len] = '\0';

        log_info("nm_cmd_listcheckpoints", "user=%s file=%s", msg->username, filename);
        handle_listcheckpoints(fd, msg->username, filename);
        return;
    }

    if (strcmp(msg->type, "EXEC") == 0) {
        char filename[256] = {0};
        size_t payload_len = strlen(msg->payload);
        size_t copy_len = (payload_len < sizeof(filename) - 1) ? payload_len : sizeof(filename) - 1;
        memcpy(filename, msg->payload, copy_len);
        filename[copy_len] = '\0';

        log_info("nm_cmd_exec", "user=%s file=%s", msg->username, filename);
        handle_exec(fd, msg->username, filename, msg->id);
        return;
    }

    if (strcmp(msg->type, "WRITE") == 0) {
        char filename[256] = {0};
        int sentence_index = 0;
        if (strlen(msg->payload) > 0) {
            char payload_copy[512];
            size_t payload_len = strlen(msg->payload);
            size_t copy_len = (payload_len < sizeof(payload_copy) - 1) ? payload_len : sizeof(payload_copy) - 1;
            memcpy(payload_copy, msg->payload, copy_len);
            payload_copy[copy_len] = '\0';
            char *sep = strchr(payload_copy, '|');
            if (sep) {
                *sep = '\0';
                sentence_index = atoi(sep + 1);
            }
            size_t fn_len = strlen(payload_copy);
            size_t fn_copy = (fn_len < sizeof(filename) - 1) ? fn_len : sizeof(filename) - 1;
            memcpy(filename, payload_copy, fn_copy);
            filename[fn_copy] = '\0';
        }
        
        log_info("nm_cmd_write", "user=%s file=%s sentence=%d",
                 msg->username, filename, sentence_index);
        handle_write(fd, msg->username, filename, sentence_index);
        return;
    }
    
    if (strcmp(msg->type, "ADDACCESS") == 0) {
        // Payload format: "FLAG|filename|username" (e.g., "R|test.txt|bob")
        char flag[16] = {0};
        char filename[256] = {0};
        char target_user[64] = {0};
        
        // Parse payload: split by |
        char payload_copy[512];
        size_t payload_len = strlen(msg->payload);
        size_t copy_len = (payload_len < sizeof(payload_copy) - 1) ? payload_len : sizeof(payload_copy) - 1;
        memcpy(payload_copy, msg->payload, copy_len);
        payload_copy[copy_len] = '\0';
        char *p = payload_copy;
        char *flag_end = strchr(p, '|');
        if (flag_end) {
            size_t flag_len = flag_end - p;
            if (flag_len < sizeof(flag)) {
                memcpy(flag, p, flag_len);
                flag[flag_len] = '\0';
            }
            p = flag_end + 1;
            
            char *file_end = strchr(p, '|');
            if (file_end) {
                size_t file_len = file_end - p;
                if (file_len < sizeof(filename)) {
                    memcpy(filename, p, file_len);
                    filename[file_len] = '\0';
                }
                p = file_end + 1;
                
                size_t user_len = strlen(p);
                if (user_len < sizeof(target_user)) {
                    memcpy(target_user, p, user_len);
                    target_user[user_len] = '\0';
                }
            }
        }
        
        log_info("nm_cmd_addaccess", "user=%s file=%s target=%s flag=%s", 
                 msg->username, filename, target_user, flag);
        handle_addaccess(fd, msg->username, flag, filename, target_user);
        return;
    }
    
    if (strcmp(msg->type, "REMACCESS") == 0) {
        // Payload format: "filename|username" (e.g., "test.txt|bob")
        char filename[256] = {0};
        char target_user[64] = {0};
        
        // Parse payload: split by |
        char payload_copy[512];
        size_t payload_len = strlen(msg->payload);
        size_t copy_len = (payload_len < sizeof(payload_copy) - 1) ? payload_len : sizeof(payload_copy) - 1;
        memcpy(payload_copy, msg->payload, copy_len);
        payload_copy[copy_len] = '\0';
        char *p = payload_copy;
        char *file_end = strchr(p, '|');
        if (file_end) {
            size_t file_len = file_end - p;
            if (file_len < sizeof(filename)) {
                memcpy(filename, p, file_len);
                filename[file_len] = '\0';
            }
            p = file_end + 1;
            
            size_t user_len = strlen(p);
            if (user_len < sizeof(target_user)) {
                memcpy(target_user, p, user_len);
                target_user[user_len] = '\0';
            }
        }
        
        log_info("nm_cmd_remaccess", "user=%s file=%s target=%s", 
                 msg->username, filename, target_user);
        handle_remaccess(fd, msg->username, filename, target_user);
        return;
    }
    
    // Handle CREATE_FOLDER
    if (strcmp(msg->type, "CREATE_FOLDER") == 0 || strcmp(msg->type, "CREATEFOLDER") == 0) {
        const char *folder_path = msg->payload;
        log_info("nm_cmd_createfolder", "user=%s folder=%s", msg->username, folder_path);
        handle_createfolder(fd, msg->username, folder_path);
        return;
    }
    
    // Handle MOVE
    if (strcmp(msg->type, "MOVE") == 0) {
        // Payload format: "filename|new_folder_path"
        char filename[512] = {0};
        char new_folder[512] = {0};
        
        const char *sep = strchr(msg->payload, '|');
        if (sep) {
            size_t fname_len = sep - msg->payload;
            if (fname_len < sizeof(filename)) {
                memcpy(filename, msg->payload, fname_len);
                filename[fname_len] = '\0';
            }
            
            size_t folder_len = strlen(sep + 1);
            if (folder_len < sizeof(new_folder)) {
                memcpy(new_folder, sep + 1, folder_len);
                new_folder[folder_len] = '\0';
            }
        }
        
        log_info("nm_cmd_move", "user=%s file=%s to=%s", msg->username, filename, new_folder);
        handle_move(fd, msg->username, filename, new_folder);
        return;
    }
    
    // Handle VIEWFOLDER
    if (strcmp(msg->type, "VIEWFOLDER") == 0 || strcmp(msg->type, "VIEW_FOLDER") == 0) {
        const char *folder_path = msg->payload;
        log_info("nm_cmd_viewfolder", "user=%s folder=%s", msg->username, folder_path);
        handle_viewfolder(fd, msg->username, folder_path);
        return;
    }
    
    // Handle REQUESTACCESS / RACC
    if (strcmp(msg->type, "REQUESTACCESS") == 0 || strcmp(msg->type, "RACC") == 0) {
        log_info("nm_cmd_requestaccess", "user=%s payload=%s", msg->username, msg->payload);
        handle_requestaccess(fd, msg->username, msg->payload);
        return;
    }
    
    // Handle VIEWACCESSREQUESTS / VIEWACCR
    if (strcmp(msg->type, "VIEWACCESSREQUESTS") == 0 || strcmp(msg->type, "VIEWACCR") == 0) {
        log_info("nm_cmd_viewaccessrequests", "user=%s payload=%s", msg->username, msg->payload);
        handle_viewaccessrequests(fd, msg->username, msg->payload);
        return;
    }
    
    // Handle APPROVEACCESSREQUEST / APPROVEACCR
    if (strcmp(msg->type, "APPROVEACCESSREQUEST") == 0 || strcmp(msg->type, "APPROVEACCR") == 0) {
        log_info("nm_cmd_approveaccessrequest", "user=%s payload=%s", msg->username, msg->payload);
        handle_approveaccessrequest(fd, msg->username, msg->payload);
        return;
    }
    
    // Handle DISAPPROVEACCESSREQUEST / DISACCR
    if (strcmp(msg->type, "DISAPPROVEACCESSREQUEST") == 0 || strcmp(msg->type, "DISACCR") == 0) {
        log_info("nm_cmd_disapproveaccessrequest", "user=%s payload=%s", msg->username, msg->payload);
        handle_disapproveaccessrequest(fd, msg->username, msg->payload);
        return;
    }
    
    // Unknown command
    log_error("nm_unknown_msg", "type=%s", msg->type);
    Error err = error_create(ERR_INVALID, "Unknown command: %s", msg->type);
    (void)send_error_response(fd, msg->id, msg->username, &err);
}

// Thread function: read lines, parse, and handle messages until peer closes.
static void *client_thread(void *arg) {
    // printf("DEBUG: client_thread started\n");
    ClientConnArg *c = (ClientConnArg*)arg;
    char line[MAX_LINE];
    while (g_running) {
        int n = recv_line(c->fd, line, sizeof(line));
        if (n <= 0) break;
        Message msg;
        // printf("DEBUG: line received: %s", line);
        if (proto_parse_line(line, &msg) == 0) {
            // printf("DEBUG: Received message type=%s from %s\n", msg.type, inet_ntoa(c->addr.sin_addr));
            // printf("DEBUG: message details: %s %s", msg.username, msg.payload);
            handle_message(c->fd, &c->addr, &msg);
        }
    }
    close(c->fd);
    free(c);
    return NULL;
}

static void on_sigint(int sig) { (void)sig; g_running = 0; }

int main(int argc, char **argv) {
    const char *host = "0.0.0.0"; int port = 5000;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--host") && i+1 < argc) host = argv[++i];
        else if (!strcmp(argv[i], "--port") && i+1 < argc) port = atoi(argv[++i]);
    }
    
    registry_init_persistence("registry_clients.txt");

    // Step 3: Initialize file index and LRU cache
    index_init();
    log_info("nm_index_init", "File index initialized");
    
    // Initialize access request queue
    request_queue_init();
    log_info("nm_request_queue_init", "Access request queue initialized");
    
    // Initialize and start heartbeat monitoring
    heartbeat_monitor_init();
    if (heartbeat_monitor_start() != 0) {
        log_error("nm_startup", "Failed to start heartbeat monitoring");
        return 1;
    }
    log_info("nm_startup", "Heartbeat monitoring started");
    
    // Initialize and start replication system
    replication_init();
    replication_worker_init();
    if (replication_worker_start() != 0) {
        log_error("nm_startup", "Failed to start replication worker");
        return 1;
    }
    log_info("nm_startup", "Replication system started");
    
    // Register failover callback
    heartbeat_monitor_set_failure_callback(on_ss_failure);
    log_info("nm_startup", "Failover callback registered");
    
    signal(SIGINT, on_sigint);
    int server_fd = create_server_socket(host, port);
    if (server_fd < 0) { perror("NM listen"); return 1; }
    log_info("nm_listen", "host=%s port=%d", host, port);

    while (g_running) {
        struct sockaddr_in addr; socklen_t alen = sizeof(addr);
        int fd = accept(server_fd, (struct sockaddr*)&addr, &alen);
        if (fd < 0) { if (!g_running) break; continue; }
        ClientConnArg *c = (ClientConnArg*)calloc(1, sizeof(ClientConnArg));
        c->fd = fd; c->addr = addr;
        pthread_t th; (void)pthread_create(&th, NULL, client_thread, c); pthread_detach(th);
    }
    
    // Shutdown: stop replication and heartbeat monitoring
    replication_worker_stop();
    log_info("nm_shutdown", "Replication worker stopped");
    
    heartbeat_monitor_stop();
    log_info("nm_shutdown", "Heartbeat monitoring stopped");
    
    close(server_fd);
    return 0;
}


