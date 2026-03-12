#define _POSIX_C_SOURCE 200809L
#include "replication_worker.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../common/log.h"
#include "../common/net.h"
#include "../common/protocol.h"
#include "registry.h"
#include "replication.h"

#define MAX_QUEUE_SIZE 1000

// Global state
static ReplicationJob *g_job_queue_head = NULL;
static ReplicationJob *g_job_queue_tail = NULL;
static int g_job_count = 0;
static pthread_mutex_t g_queue_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_queue_cond = PTHREAD_COND_INITIALIZER;
static pthread_t g_worker_thread;
static volatile int g_worker_running = 0;

// Statistics
static int g_completed_jobs = 0;
static int g_failed_jobs = 0;

// Initialize worker
void replication_worker_init(void) {
    pthread_mutex_lock(&g_queue_mu);
    
    // Clear queue
    ReplicationJob *current = g_job_queue_head;
    while (current) {
        ReplicationJob *next = current->next;
        free(current);
        current = next;
    }
    
    g_job_queue_head = NULL;
    g_job_queue_tail = NULL;
    g_job_count = 0;
    g_completed_jobs = 0;
    g_failed_jobs = 0;
    g_worker_running = 0;
    
    pthread_mutex_unlock(&g_queue_mu);
    
    log_info("replication_worker_init", "Worker initialized");
}

// Process a single replication job
static int process_job(const ReplicationJob *job) {
    log_info("replication_worker_process", "op=%d file=%s primary=%s replica=%s",
             job->operation, job->filename, job->primary_ss, job->replica_ss);
    
    // Get SS connection info
    char primary_host[64], replica_host[64];
    int primary_port, replica_port;
    
    if (registry_get_ss_info(job->primary_ss, primary_host, sizeof(primary_host), &primary_port) != 0) {
        log_error("replication_worker_error", "Primary SS %s not found in registry", job->primary_ss);
        return -1;
    }
    
    if (registry_get_ss_info(job->replica_ss, replica_host, sizeof(replica_host), &replica_port) != 0) {
        log_error("replication_worker_error", "Replica SS %s not found in registry", job->replica_ss);
        return -1;
    }
    
    if (job->operation == REPL_OP_CREATE || job->operation == REPL_OP_UPDATE) {
        // Step 1: Fetch file content from primary
        int primary_fd = connect_to_host(primary_host, primary_port);
        if (primary_fd < 0) {
            log_error("replication_worker_fetch", "Failed to connect to primary %s:%d", 
                     primary_host, primary_port);
            return -1;
        }
        
        // Send GET_FILE_CONTENT request
        Message get_msg = {0};
        snprintf(get_msg.type, sizeof(get_msg.type), "GET_FILE_CONTENT");
        snprintf(get_msg.id, sizeof(get_msg.id), "repl_%ld", (long)time(NULL));
        snprintf(get_msg.username, sizeof(get_msg.username), "NM");
        snprintf(get_msg.role, sizeof(get_msg.role), "NM");
        snprintf(get_msg.payload, sizeof(get_msg.payload), "%s", job->filename);
        
        char get_line[MAX_LINE];
        proto_format_line(&get_msg, get_line, sizeof(get_line));
        send_all(primary_fd, get_line, strlen(get_line));
        
        // Receive file content
        char *content = NULL;
        size_t content_size = 0;
        size_t content_capacity = 4096;
        content = (char*)malloc(content_capacity);
        if (!content) {
            close(primary_fd);
            log_error("replication_worker_fetch", "Memory allocation failed");
            return -1;
        }
        
        char line[MAX_LINE];
        while (1) {
            int n = recv_line(primary_fd, line, sizeof(line));
            if (n <= 0) break;
            
            Message msg;
            if (proto_parse_line(line, &msg) != 0) continue;
            
            if (strcmp(msg.type, "STOP") == 0) {
                break;
            }
            
            if (strcmp(msg.type, "ERROR") == 0) {
                log_error("replication_worker_fetch", "Primary returned error: %s", msg.payload);
                free(content);
                close(primary_fd);
                return -1;
            }
            
            if (strcmp(msg.type, "DATA") == 0) {
                size_t payload_len = strlen(msg.payload);
                
                // Ensure capacity
                while (content_size + payload_len + 1 > content_capacity) {
                    content_capacity *= 2;
                    char *new_content = (char*)realloc(content, content_capacity);
                    if (!new_content) {
                        free(content);
                        close(primary_fd);
                        log_error("replication_worker_fetch", "Memory reallocation failed");
                        return -1;
                    }
                    content = new_content;
                }
                
                // Copy and decode (\x01 back to \n)
                for (size_t i = 0; i < payload_len; i++) {
                    if (msg.payload[i] == '\x01') {
                        content[content_size++] = '\n';
                    } else {
                        content[content_size++] = msg.payload[i];
                    }
                }
            }
        }
        
        close(primary_fd);
        content[content_size] = '\0';
        
        log_info("replication_worker_fetched", "file=%s size=%zu from %s", 
                 job->filename, content_size, job->primary_ss);
        
        // Step 2: Write file content to replica
        int replica_fd = connect_to_host(replica_host, replica_port);
        if (replica_fd < 0) {
            log_error("replication_worker_push", "Failed to connect to replica %s:%d", 
                     replica_host, replica_port);
            free(content);
            return -1;
        }
        
        // Send PUT_FILE_CONTENT request
        Message put_msg = {0};
        snprintf(put_msg.type, sizeof(put_msg.type), "PUT_FILE_CONTENT");
        snprintf(put_msg.id, sizeof(put_msg.id), "repl_%ld", (long)time(NULL));
        snprintf(put_msg.username, sizeof(put_msg.username), "NM");
        snprintf(put_msg.role, sizeof(put_msg.role), "NM");
        snprintf(put_msg.payload, sizeof(put_msg.payload), "%s", job->filename);
        
        char put_line[MAX_LINE];
        proto_format_line(&put_msg, put_line, sizeof(put_line));
        send_all(replica_fd, put_line, strlen(put_line));
        
        // Send content in DATA messages
        size_t offset = 0;
        while (offset < content_size) {
            Message data_msg = {0};
            snprintf(data_msg.type, sizeof(data_msg.type), "DATA");
            snprintf(data_msg.id, sizeof(data_msg.id), "repl_%ld", (long)time(NULL));
            snprintf(data_msg.username, sizeof(data_msg.username), "NM");
            snprintf(data_msg.role, sizeof(data_msg.role), "NM");
            
            size_t payload_pos = 0;
            size_t payload_max = sizeof(data_msg.payload) - 1;
            
            while (offset < content_size && payload_pos < payload_max) {
                if (content[offset] == '\n') {
                    data_msg.payload[payload_pos++] = '\x01';
                } else {
                    data_msg.payload[payload_pos++] = content[offset];
                }
                offset++;
            }
            data_msg.payload[payload_pos] = '\0';
            
            char data_line[MAX_LINE];
            proto_format_line(&data_msg, data_line, sizeof(data_line));
            send_all(replica_fd, data_line, strlen(data_line));
        }
        
        // Send STOP
        Message stop_msg = {0};
        snprintf(stop_msg.type, sizeof(stop_msg.type), "STOP");
        snprintf(stop_msg.id, sizeof(stop_msg.id), "repl_%ld", (long)time(NULL));
        snprintf(stop_msg.username, sizeof(stop_msg.username), "NM");
        snprintf(stop_msg.role, sizeof(stop_msg.role), "NM");
        
        char stop_line[MAX_LINE];
        proto_format_line(&stop_msg, stop_line, sizeof(stop_line));
        send_all(replica_fd, stop_line, strlen(stop_line));
        
        // Wait for ACK
        int n = recv_line(replica_fd, line, sizeof(line));
        if (n > 0) {
            Message ack_msg;
            if (proto_parse_line(line, &ack_msg) == 0) {
                if (strcmp(ack_msg.type, "ACK") == 0) {
                    log_info("replication_worker_success", "file=%s replicated to %s", 
                             job->filename, job->replica_ss);
                    replication_mark_synced(job->primary_ss, job->replica_ss);
                } else if (strcmp(ack_msg.type, "ERROR") == 0) {
                    log_error("replication_worker_push", "Replica returned error: %s", ack_msg.payload);
                    free(content);
                    close(replica_fd);
                    return -1;
                }
            }
        }
        
        free(content);
        close(replica_fd);
        
        // Also replicate metadata file (.meta)
        char meta_filename[MAX_REPL_FILENAME + 10];
        snprintf(meta_filename, sizeof(meta_filename), "%s.meta", job->filename);
        
        // Try to fetch metadata from primary
        primary_fd = connect_to_host(primary_host, primary_port);
        if (primary_fd >= 0) {
            Message meta_req = {0};
            snprintf(meta_req.type, sizeof(meta_req.type), "GET_FILE_CONTENT");
            snprintf(meta_req.id, sizeof(meta_req.id), "repl_meta_%ld", (long)time(NULL));
            snprintf(meta_req.username, sizeof(meta_req.username), "NM");
            snprintf(meta_req.role, sizeof(meta_req.role), "NM");
            snprintf(meta_req.payload, sizeof(meta_req.payload), "metadata/%s", meta_filename);
            
            char meta_line[MAX_LINE];
            proto_format_line(&meta_req, meta_line, sizeof(meta_line));
            send_all(primary_fd, meta_line, strlen(meta_line));
            
            char meta_resp[MAX_LINE];
            if (recv_line(primary_fd, meta_resp, sizeof(meta_resp)) > 0) {
                Message meta_msg;
                if (proto_parse_line(meta_resp, &meta_msg) == 0 && strcmp(meta_msg.type, "ACK") == 0) {
                    size_t meta_size = (size_t)strtoull(meta_msg.payload, NULL, 10);
                    if (meta_size > 0 && meta_size < 65536) {
                        char *meta_content = (char*)malloc(meta_size + 1);
                        if (meta_content) {
                            size_t meta_received = 0;
                            while (meta_received < meta_size) {
                                char meta_chunk[MAX_LINE];
                                int meta_n = recv_line(primary_fd, meta_chunk, sizeof(meta_chunk));
                                if (meta_n <= 0) break;
                                Message meta_data;
                                if (proto_parse_line(meta_chunk, &meta_data) == 0 && 
                                    strcmp(meta_data.type, "DATA") == 0) {
                                    size_t chunk_len = strlen(meta_data.payload);
                                    if (meta_received + chunk_len <= meta_size) {
                                        memcpy(meta_content + meta_received, meta_data.payload, chunk_len);
                                        meta_received += chunk_len;
                                    }
                                }
                            }
                            meta_content[meta_received] = '\0';
                            
                            // Push metadata to replica
                            replica_fd = connect_to_host(replica_host, replica_port);
                            if (replica_fd >= 0) {
                                Message meta_put = {0};
                                snprintf(meta_put.type, sizeof(meta_put.type), "PUT_FILE_CONTENT");
                                snprintf(meta_put.id, sizeof(meta_put.id), "repl_meta_%ld", (long)time(NULL));
                                snprintf(meta_put.username, sizeof(meta_put.username), "NM");
                                snprintf(meta_put.role, sizeof(meta_put.role), "NM");
                                snprintf(meta_put.payload, sizeof(meta_put.payload), "metadata/%s|%zu", meta_filename, meta_size);
                                
                                char meta_put_line[MAX_LINE];
                                proto_format_line(&meta_put, meta_put_line, sizeof(meta_put_line));
                                send_all(replica_fd, meta_put_line, strlen(meta_put_line));
                                
                                size_t meta_sent = 0;
                                while (meta_sent < meta_size) {
                                    size_t chunk_size = (meta_size - meta_sent > 1024) ? 1024 : (meta_size - meta_sent);
                                    Message meta_data = {0};
                                    snprintf(meta_data.type, sizeof(meta_data.type), "DATA");
                                    snprintf(meta_data.id, sizeof(meta_data.id), "repl_meta_%ld", (long)time(NULL));
                                    memcpy(meta_data.payload, meta_content + meta_sent, chunk_size);
                                    meta_data.payload[chunk_size] = '\0';
                                    
                                    char meta_data_line[MAX_LINE];
                                    proto_format_line(&meta_data, meta_data_line, sizeof(meta_data_line));
                                    send_all(replica_fd, meta_data_line, strlen(meta_data_line));
                                    meta_sent += chunk_size;
                                }
                                
                                close(replica_fd);
                            }
                            free(meta_content);
                        }
                    }
                }
            }
            close(primary_fd);
        }
        
        return 0;
        
    } else if (job->operation == REPL_OP_DELETE) {
        // Connect to replica and send DELETE command
        int replica_fd = connect_to_host(replica_host, replica_port);
        if (replica_fd < 0) {
            log_error("replication_worker_delete", "Failed to connect to replica %s:%d", 
                     replica_host, replica_port);
            return -1;
        }
        
        Message del_msg = {0};
        snprintf(del_msg.type, sizeof(del_msg.type), "DELETE");
        snprintf(del_msg.id, sizeof(del_msg.id), "repl_%ld", (long)time(NULL));
        snprintf(del_msg.username, sizeof(del_msg.username), "NM");
        snprintf(del_msg.role, sizeof(del_msg.role), "NM");
        snprintf(del_msg.payload, sizeof(del_msg.payload), "%s", job->filename);
        
        char del_line[MAX_LINE];
        proto_format_line(&del_msg, del_line, sizeof(del_line));
        send_all(replica_fd, del_line, strlen(del_line));
        
        // Wait for ACK
        char line[MAX_LINE];
        int n = recv_line(replica_fd, line, sizeof(line));
        if (n > 0) {
            Message ack_msg;
            if (proto_parse_line(line, &ack_msg) == 0) {
                if (strcmp(ack_msg.type, "ACK") == 0) {
                    log_info("replication_worker_delete_success", "file=%s deleted from %s", 
                             job->filename, job->replica_ss);
                }
            }
        }
        
        close(replica_fd);
        
        // Also delete metadata file (.meta)
        char meta_filename[MAX_REPL_FILENAME + 10];
        snprintf(meta_filename, sizeof(meta_filename), "%s.meta", job->filename);
        
        replica_fd = connect_to_host(replica_host, replica_port);
        if (replica_fd >= 0) {
            Message meta_del = {0};
            snprintf(meta_del.type, sizeof(meta_del.type), "DELETE");
            snprintf(meta_del.id, sizeof(meta_del.id), "repl_meta_%ld", (long)time(NULL));
            snprintf(meta_del.username, sizeof(meta_del.username), "NM");
            snprintf(meta_del.role, sizeof(meta_del.role), "NM");
            snprintf(meta_del.payload, sizeof(meta_del.payload), "metadata/%s", meta_filename);
            
            char meta_del_line[MAX_LINE];
            proto_format_line(&meta_del, meta_del_line, sizeof(meta_del_line));
            send_all(replica_fd, meta_del_line, strlen(meta_del_line));
            close(replica_fd);
        }
        
        return 0;
    }
    
    log_warning("replication_worker_unsupported", "Operation %d not yet implemented", job->operation);
    return -1;
}

// Worker thread function
static void *worker_thread_func(void *arg) {
    (void)arg;
    
    log_info("replication_worker_thread", "Worker thread started");
    
    while (g_worker_running) {
        pthread_mutex_lock(&g_queue_mu);
        
        // Wait for jobs
        while (g_worker_running && g_job_queue_head == NULL) {
            pthread_cond_wait(&g_queue_cond, &g_queue_mu);
        }
        
        if (!g_worker_running) {
            pthread_mutex_unlock(&g_queue_mu);
            break;
        }
        
        // Dequeue job
        ReplicationJob *job = g_job_queue_head;
        if (job) {
            g_job_queue_head = job->next;
            if (g_job_queue_head == NULL) {
                g_job_queue_tail = NULL;
            }
            g_job_count--;
        }
        
        pthread_mutex_unlock(&g_queue_mu);
        
        if (job) {
            // Process job
            if (process_job(job) == 0) {
                pthread_mutex_lock(&g_queue_mu);
                g_completed_jobs++;
                pthread_mutex_unlock(&g_queue_mu);
            } else {
                pthread_mutex_lock(&g_queue_mu);
                g_failed_jobs++;
                pthread_mutex_unlock(&g_queue_mu);
            }
            
            free(job);
        }
    }
    
    log_info("replication_worker_thread", "Worker thread stopped");
    return NULL;
}

// Start worker thread
int replication_worker_start(void) {
    if (g_worker_running) {
        log_warning("replication_worker_start", "Worker already running");
        return 0;
    }
    
    g_worker_running = 1;
    
    int rc = pthread_create(&g_worker_thread, NULL, worker_thread_func, NULL);
    if (rc != 0) {
        g_worker_running = 0;
        log_error("replication_worker_start", "Failed to create worker thread: %d", rc);
        return -1;
    }
    
    log_info("replication_worker_start", "Replication worker started");
    return 0;
}

// Stop worker thread
void replication_worker_stop(void) {
    if (!g_worker_running) return;
    
    log_info("replication_worker_stop", "Stopping worker thread...");
    
    g_worker_running = 0;
    
    // Wake up worker thread
    pthread_cond_signal(&g_queue_cond);
    
    // Wait for thread to finish
    pthread_join(g_worker_thread, NULL);
    
    log_info("replication_worker_stop", "Worker thread stopped");
}

// Queue a replication job
int replication_worker_queue(ReplicationOp operation,
                              const char *filename,
                              const char *primary_ss,
                              const char *replica_ss) {
    if (!filename || !primary_ss || !replica_ss) return -1;
    
    pthread_mutex_lock(&g_queue_mu);
    
    // Check queue size
    if (g_job_count >= MAX_QUEUE_SIZE) {
        pthread_mutex_unlock(&g_queue_mu);
        log_error("replication_worker_queue", "Queue full (%d jobs)", g_job_count);
        return -1;
    }
    
    // Create job
    ReplicationJob *job = (ReplicationJob *)calloc(1, sizeof(ReplicationJob));
    if (!job) {
        pthread_mutex_unlock(&g_queue_mu);
        log_error("replication_worker_queue", "Memory allocation failed");
        return -1;
    }
    
    job->operation = operation;
    strncpy(job->filename, filename, sizeof(job->filename) - 1);
    strncpy(job->primary_ss, primary_ss, sizeof(job->primary_ss) - 1);
    strncpy(job->replica_ss, replica_ss, sizeof(job->replica_ss) - 1);
    job->next = NULL;
    
    // Enqueue
    if (g_job_queue_tail) {
        g_job_queue_tail->next = job;
    } else {
        g_job_queue_head = job;
    }
    g_job_queue_tail = job;
    g_job_count++;
    
    pthread_mutex_unlock(&g_queue_mu);
    
    // Signal worker
    pthread_cond_signal(&g_queue_cond);
    
    log_info("replication_worker_queued", "op=%d file=%s primary=%s replica=%s queued=%d",
             operation, filename, primary_ss, replica_ss, g_job_count);
    
    return 0;
}

// Get statistics
void replication_worker_get_stats(ReplicationStats *stats) {
    if (!stats) return;
    
    pthread_mutex_lock(&g_queue_mu);
    stats->pending_jobs = g_job_count;
    stats->completed_jobs = g_completed_jobs;
    stats->failed_jobs = g_failed_jobs;
    pthread_mutex_unlock(&g_queue_mu);
}
