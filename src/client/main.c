// Client: Interactive shell for file operations
// Phase 2: Supports VIEW, CREATE, READ, DELETE, INFO, LIST commands
#define _POSIX_C_SOURCE 200809L  // For strdup
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "../common/net.h"
#include "../common/log.h"
#include "../common/protocol.h"
#include "../common/errors.h"
#include "commands.h"

// Global: connection to NM (kept open for entire session)
static int g_nm_fd = -1;
static char g_username[64] = {0};

// Forward declaration
static int handle_ss_command(const ParsedCommand *cmd, const char *ss_host, int ss_port);
static int perform_write_session(const ParsedCommand *cmd, const char *ss_host, int ss_port);
static int perform_undo_command(const ParsedCommand *cmd, const char *ss_host, int ss_port);
static void print_payload_with_newlines(const char *payload);
static void print_help_message(void);

// Send command to NM and receive response
// Returns: 0 on success, -1 on error
static int send_command_and_receive(const ParsedCommand *cmd) {
    if (!cmd || g_nm_fd < 0) return -1;
    
    // Format command message
    char msg_buf[MAX_LINE];
    if (format_command_message(cmd, g_username, msg_buf, sizeof(msg_buf)) != 0) {
        printf("Error: Failed to format command\n");
        return -1;
    }
    
    // Send to NM
    if (send_all(g_nm_fd, msg_buf, strlen(msg_buf)) != 0) {
        printf("Error: Failed to send command to NM\n");
        return -1;
    }
    // printf("DEBUG: Sent command: %s", msg_buf);
    
    // Receive response
    char resp_buf[MAX_LINE];
    int n = recv_line(g_nm_fd, resp_buf, sizeof(resp_buf));
    if (n <= 0) {
        printf("Error: Failed to receive response from NM\n");
        fflush(stdout);
        return -1;
    }
    
    // Parse response
    Message resp;
    if (proto_parse_line(resp_buf, &resp) != 0) {
        printf("Error: Failed to parse response\n");
        fflush(stdout);
        return -1;
    }
    
    // Handle response based on type
    if (strcmp(resp.type, "ERROR") == 0) {
        // Error response - parse error code and message
        char error_code[64];
        char error_msg[256];
        if (proto_parse_error(&resp, error_code, sizeof(error_code),
                              error_msg, sizeof(error_msg)) == 0) {
            printf("ERROR [%s]: %s\n", error_code, error_msg);
        } else {
            printf("ERROR: %s\n", resp.payload);
        }
        fflush(stdout);
    } else if (strcmp(resp.type, "ACK") == 0) {
        // Success response - print payload if non-empty, otherwise print a generic success message
        
        // Special handling for LISTCHECKPOINTS - format nicely
        if (strcmp(cmd->cmd, "LISTCHECKPOINTS") == 0 && strlen(resp.payload) > 0) {
            // Payload format: tag|creator|timestamp|filesize (one per line)
            // Newlines are encoded as \x01
            if (strstr(resp.payload, "No checkpoints") != NULL) {
                printf("%s\n", resp.payload);
            } else {
                printf("Checkpoints for file '%s':\n", cmd->args[0]);
                printf("------------------------------------------------------------\n");
                printf("%-20s %-15s %-20s %s\n", "Tag", "Creator", "Created", "Size");
                printf("------------------------------------------------------------\n");
                
                char *payload_copy = strdup(resp.payload);
                if (payload_copy) {
                    // Convert \x01 back to \n
                    for (char *p = payload_copy; *p; p++) {
                        if (*p == '\x01') *p = '\n';
                    }
                    
                    char *saveptr = NULL;
                    char *line = strtok_r(payload_copy, "\n", &saveptr);
                    while (line) {
                        char tag[64] = {0}, creator[64] = {0}, timestamp[32] = {0};
                        size_t filesize = 0;
                        
                        // Parse: tag|creator|timestamp|filesize
                        char *field_saveptr = NULL;
                        char *field = strtok_r(line, "|", &field_saveptr);
                        if (field) strncpy(tag, field, sizeof(tag) - 1);
                        field = strtok_r(NULL, "|", &field_saveptr);
                        if (field) strncpy(creator, field, sizeof(creator) - 1);
                        field = strtok_r(NULL, "|", &field_saveptr);
                        if (field) strncpy(timestamp, field, sizeof(timestamp) - 1);
                        field = strtok_r(NULL, "|", &field_saveptr);
                        if (field) filesize = atoll(field);
                        
                        printf("%-20s %-15s %-20s %zu bytes\n", tag, creator, timestamp, filesize);
                        line = strtok_r(NULL, "\n", &saveptr);
                    }
                    free(payload_copy);
                }
                printf("------------------------------------------------------------\n");
            }
        }
        else if (strlen(resp.payload) > 0) {
            printf("%s\n", resp.payload);
        } else {
            printf("Success\n");
        }
        fflush(stdout);
    } else if (strcmp(resp.type, "DATA") == 0) {
        print_payload_with_newlines(resp.payload);
        // READ, VIEWCHECKPOINT, and EXEC commands use DATA streaming
        if (strcmp(cmd->cmd, "EXEC") == 0 || strcmp(cmd->cmd, "VIEWCHECKPOINT") == 0) {
            while (1) {
                int n = recv_line(g_nm_fd, resp_buf, sizeof(resp_buf));
                if (n <= 0) break;
                if (proto_parse_line(resp_buf, &resp) != 0) break;
                if (strcmp(resp.type, "STOP") == 0) {
                    break;
                } else if (strcmp(resp.type, "DATA") == 0) {
                    print_payload_with_newlines(resp.payload);
                } else if (strcmp(resp.type, "ERROR") == 0) {
                    char error_code[64];
                    char error_msg[256];
                    if (proto_parse_error(&resp, error_code, sizeof(error_code),
                                          error_msg, sizeof(error_msg)) == 0) {
                        printf("ERROR [%s]: %s\n", error_code, error_msg);
                    } else {
                        printf("ERROR: %s\n", resp.payload);
                    }
                    break;
                }
            }
        }
        fflush(stdout);
        return 0;  // Return after handling DATA streaming to avoid fall-through
    } else if (strcmp(resp.type, "STOP") == 0) {
        // Standalone STOP message (shouldn't normally happen, but handle gracefully)
        fflush(stdout);
        return 0;
    } else if (strcmp(resp.type, "SS_INFO") == 0) {
        // SS_INFO response - contains SS connection info for direct connection
        // Payload format: "host=IP,port=PORT"
        // Parse and connect to SS
        char ss_host[64] = {0};
        int ss_port = 0;
        
        // Parse payload: "host=IP,port=PORT"
        char *host_start = strstr(resp.payload, "host=");
        char *port_start = strstr(resp.payload, "port=");
        
        if (host_start) {
            char *host_end = strchr(host_start + 5, ',');
            if (host_end) {
                size_t host_len = host_end - (host_start + 5);
                if (host_len < sizeof(ss_host)) {
                    memcpy(ss_host, host_start + 5, host_len);
                    ss_host[host_len] = '\0';
                }
            } else {
                // No comma, host is at end
                size_t host_len = strlen(host_start + 5);
                if (host_len < sizeof(ss_host)) {
                    memcpy(ss_host, host_start + 5, host_len);
                    ss_host[host_len] = '\0';
                }
            }
        }
        
        if (port_start) {
            ss_port = atoi(port_start + 5);
        }
        
        if (strlen(ss_host) > 0 && ss_port > 0) {
            // Connect to SS and handle command
            return handle_ss_command(cmd, ss_host, ss_port);
        } else {
            printf("Error: Invalid SS connection info\n");
            fflush(stdout);
            return -1;
        }
    } else {
        // Unknown response type
        printf("Response: type=%s payload=%s\n", resp.type, resp.payload);
        fflush(stdout);
    }
    
    return 0;
}

// Handle direct SS command (READ, STREAM)
// Connects to SS, sends command, receives data until STOP
// Returns: 0 on success, -1 on error
static int handle_ss_command(const ParsedCommand *cmd, const char *ss_host, int ss_port) {
    if (!cmd || !ss_host || ss_port <= 0) return -1;

    if (strcmp(cmd->cmd, "WRITE") == 0) {
        return perform_write_session(cmd, ss_host, ss_port);
    }
    if (strcmp(cmd->cmd, "UNDO") == 0) {
        return perform_undo_command(cmd, ss_host, ss_port);
    }
    
    // Connect to SS
    int ss_fd = connect_to_host(ss_host, ss_port);
    if (ss_fd < 0) {
        printf("Error: Failed to connect to storage server at %s:%d\n", ss_host, ss_port);
        fflush(stdout);
        return -1;
    }
    
    // Format command message for SS
    Message ss_cmd = {0};
    size_t cmd_len = strlen(cmd->cmd);
    size_t copy_len = (cmd_len < sizeof(ss_cmd.type) - 1) ? cmd_len : sizeof(ss_cmd.type) - 1;
    memcpy(ss_cmd.type, cmd->cmd, copy_len);
    ss_cmd.type[copy_len] = '\0';
    
    (void)snprintf(ss_cmd.id, sizeof(ss_cmd.id), "%ld", (long)time(NULL));
    
    size_t username_len = strlen(g_username);
    copy_len = (username_len < sizeof(ss_cmd.username) - 1) ? username_len : sizeof(ss_cmd.username) - 1;
    memcpy(ss_cmd.username, g_username, copy_len);
    ss_cmd.username[copy_len] = '\0';
    
    memcpy(ss_cmd.role, "CLIENT", 6);
    ss_cmd.role[6] = '\0';
    
    // Payload is filename (first argument)
    if (cmd->argc > 0) {
        size_t arg_len = strlen(cmd->args[0]);
        copy_len = (arg_len < sizeof(ss_cmd.payload) - 1) ? arg_len : sizeof(ss_cmd.payload) - 1;
        memcpy(ss_cmd.payload, cmd->args[0], copy_len);
        ss_cmd.payload[copy_len] = '\0';
    } else {
        ss_cmd.payload[0] = '\0';
    }
    
    // Send command to SS
    char cmd_buf[MAX_LINE];
    if (proto_format_line(&ss_cmd, cmd_buf, sizeof(cmd_buf)) != 0) {
        printf("Error: Failed to format command for SS\n");
        fflush(stdout);
        close(ss_fd);
        return -1;
    }
    
    if (send_all(ss_fd, cmd_buf, strlen(cmd_buf)) != 0) {
        printf("Error: Failed to send command to SS\n");
        fflush(stdout);
        close(ss_fd);
        return -1;
    }
    
    // Handle response based on command type
    if (strcmp(cmd->cmd, "READ") == 0) {
        // Receive data until STOP packet
        while (1) {
            char resp_buf[MAX_LINE];
            int n = recv_line(ss_fd, resp_buf, sizeof(resp_buf));
            if (n <= 0) {
                printf("Error: Connection closed unexpectedly\n");
                fflush(stdout);
                close(ss_fd);
                return -1;
            }
            
            // Parse response
            Message resp;
            if (proto_parse_line(resp_buf, &resp) != 0) {
                printf("Error: Failed to parse response from SS\n");
                fflush(stdout);
                close(ss_fd);
                return -1;
            }
            
            // Check for STOP packet
            if (strcmp(resp.type, "STOP") == 0) {
                // End of data
                break;
            }
            
            // Check for ERROR
            if (strcmp(resp.type, "ERROR") == 0) {
                char error_code[64];
                char error_msg[256];
                if (proto_parse_error(&resp, error_code, sizeof(error_code),
                                      error_msg, sizeof(error_msg)) == 0) {
                    printf("ERROR [%s]: %s\n", error_code, error_msg);
                } else {
                    printf("ERROR: %s\n", resp.payload);
                }
                fflush(stdout);
                close(ss_fd);
                return -1;
            }
            
            // Handle DATA packet
            if (strcmp(resp.type, "DATA") == 0) {
                // Convert \x01 (SOH) back to \n
                if (strlen(resp.payload) > 0) {
                    char *p = resp.payload;
                    while (*p) {
                        if (*p == '\x01') {
                            putchar('\n');
                        } else {
                            putchar(*p);
                        }
                        p++;
                    }
                }
            }
        }
        
        // Add final newline if needed
        printf("\n");
        fflush(stdout);
    } else if (strcmp(cmd->cmd, "STREAM") == 0) {
        // Receive words until STOP packet
        int first_word = 1;
        while (1) {
            char resp_buf[MAX_LINE];
            int n = recv_line(ss_fd, resp_buf, sizeof(resp_buf));
            if (n <= 0) {
                printf("\nError: Connection closed unexpectedly\n");
                fflush(stdout);
                close(ss_fd);
                return -1;
            }
            
            // Parse response
            Message resp;
            if (proto_parse_line(resp_buf, &resp) != 0) {
                printf("\nError: Failed to parse response from SS\n");
                fflush(stdout);
                close(ss_fd);
                return -1;
            }
            
            // Check for STOP packet
            if (strcmp(resp.type, "STOP") == 0) {
                // End of stream
                printf("\n");
                fflush(stdout);
                break;
            }
            
            // Check for ERROR
            if (strcmp(resp.type, "ERROR") == 0) {
                char error_code[64];
                char error_msg[256];
                if (proto_parse_error(&resp, error_code, sizeof(error_code),
                                      error_msg, sizeof(error_msg)) == 0) {
                    printf("\nERROR [%s]: %s\n", error_code, error_msg);
                } else {
                    printf("\nERROR: %s\n", resp.payload);
                }
                fflush(stdout);
                close(ss_fd);
                return -1;
            }
            
            // Handle DATA packet (word)
            if (strcmp(resp.type, "DATA") == 0) {
                // Print word with space before it (except first word)
                if (!first_word) {
                    putchar(' ');
                }
                printf("%s", resp.payload);
                fflush(stdout);
                first_word = 0;
            }
        }
    }
    
    close(ss_fd);
    return 0;
}

static void print_payload_with_newlines(const char *payload) {
    if (!payload) return;
    for (const char *p = payload; *p; p++) {
        if (*p == '\x01') {
            putchar('\n');
        } else {
            putchar(*p);
        }
    }
    if (strlen(payload) > 0) {
        putchar('\n');
    }
}

static void print_help_message(void) {
    printf("Available commands:\n");
    printf("  VIEW [-a] [-l]                List files (accessible/all, optionally detailed)\n");
    printf("  CREATE <file>                 Create an empty file\n");
    printf("  DELETE <file>                 Delete a file you own\n");
    printf("  INFO <file>                   Show metadata for a file\n");
    printf("  LIST                          List all registered users\n");
    printf("  READ <file>                   Read entire file content\n");
    printf("  STREAM <file>                 Stream file word-by-word (0.1s delay)\n");
    printf("  WRITE <file> <sentence>       Lock a sentence and edit words interactively\n");
    printf("  UNDO <file>                   Revert the last WRITE (requires write access)\n");
    printf("  ADDACCESS -R/-W <file> <user> Grant read or read/write access\n");
    printf("  REMACCESS <file> <user>       Remove access\n");
    printf("  EXEC <file>                   Execute the file as a bash script\n");
    printf("  CHECKPOINT/VIEWCHECKPOINT/REVERT/LISTCHECKPOINTS <file> [tag]\n");
    printf("                                Manage checkpoints\n");
    printf("  CREATEFOLDER <path>           Create folder (bonus)\n");
    printf("  VIEWFOLDER <path>             List contents of a folder\n");
    printf("  MOVE <file> <folder>          Move file into folder\n");
    printf("  RACC / VIEWACCR / APPROVEACCR / DISACCR ...\n");
    printf("                                Access request workflow commands\n");
    printf("  HELP                          Show this help message\n");
    printf("  EXIT                          Quit the client\n\n");
    printf("WRITE usage: enter multiple '<word_index> <content>' lines (0-based). Use ETIRW to commit.\n");
}

static int perform_write_session(const ParsedCommand *cmd, const char *ss_host, int ss_port) {
    if (cmd->argc < 2) {
        printf("Error: WRITE requires filename and sentence index\n");
        fflush(stdout);
        return -1;
    }
    const char *filename = cmd->args[0];
    const char *sentence_arg = cmd->args[1];
    int ss_fd = connect_to_host(ss_host, ss_port);
    if (ss_fd < 0) {
        printf("Error: Failed to connect to storage server at %s:%d\n", ss_host, ss_port);
        fflush(stdout);
        return -1;
    }

    Message write_msg = {0};
    (void)snprintf(write_msg.type, sizeof(write_msg.type), "%s", "WRITE");
    (void)snprintf(write_msg.id, sizeof(write_msg.id), "%ld", (long)time(NULL));
    (void)snprintf(write_msg.username, sizeof(write_msg.username), "%s", g_username);
    (void)snprintf(write_msg.role, sizeof(write_msg.role), "%s", "CLIENT");
    (void)snprintf(write_msg.payload, sizeof(write_msg.payload), "%s|%s", filename, sentence_arg);
    
    char line_buf[MAX_LINE];
    if (proto_format_line(&write_msg, line_buf, sizeof(line_buf)) != 0 ||
        send_all(ss_fd, line_buf, strlen(line_buf)) != 0) {
        printf("Error: Failed to send WRITE request to SS\n");
        fflush(stdout);
        close(ss_fd);
        return -1;
    }

    int ready = 0;
    while (!ready) {
        int n = recv_line(ss_fd, line_buf, sizeof(line_buf));
        if (n <= 0) {
            printf("Error: No response from storage server\n");
            fflush(stdout);
            close(ss_fd);
            return -1;
        }
        Message resp;
        if (proto_parse_line(line_buf, &resp) != 0) {
            printf("Error: Failed to parse response from SS\n");
            fflush(stdout);
            close(ss_fd);
            return -1;
        }
        if (strcmp(resp.type, "ERROR") == 0) {
            char error_code[64];
            char error_msg[256];
            if (proto_parse_error(&resp, error_code, sizeof(error_code),
                                  error_msg, sizeof(error_msg)) == 0) {
                printf("ERROR [%s]: %s\n", error_code, error_msg);
            } else {
                printf("ERROR: %s\n", resp.payload);
            }
            fflush(stdout);
            close(ss_fd);
            return -1;
        }
        if (strcmp(resp.type, "WRITE_READY") == 0) {
            printf("WRITE session ready for '%s' (sentence %s)\n", filename, sentence_arg);
            if (strlen(resp.payload) > 0) {
                printf("Current sentence:\n");
                print_payload_with_newlines(resp.payload);
            }
            ready = 1;
            break;
        }
        // Unexpected data prior to ready; show payload and continue
        print_payload_with_newlines(resp.payload);
    }

    printf("Enter '<word_index> <content>' lines (0-based indices). Type ETIRW to finish.\n");
    fflush(stdout);

    char input[MAX_CMD_LINE];
    while (1) {
        printf("WRITE> ");
        fflush(stdout);
        if (!fgets(input, sizeof(input), stdin)) {
            printf("\nAborting WRITE (no input)\n");
            fflush(stdout);
            Message abort_msg = {0};
            (void)snprintf(abort_msg.type, sizeof(abort_msg.type), "%s", "WRITE_ABORT");
            (void)snprintf(abort_msg.id, sizeof(abort_msg.id), "%ld", (long)time(NULL));
            (void)snprintf(abort_msg.username, sizeof(abort_msg.username), "%s", g_username);
            (void)snprintf(abort_msg.role, sizeof(abort_msg.role), "%s", "CLIENT");
            abort_msg.payload[0] = '\0';
            if (proto_format_line(&abort_msg, line_buf, sizeof(line_buf)) == 0) {
                send_all(ss_fd, line_buf, strlen(line_buf));
            }
            close(ss_fd);
            return -1;
        }
        // trim newline
        size_t len = strlen(input);
        while (len > 0 && (input[len-1] == '\n' || input[len-1] == '\r')) {
            input[--len] = '\0';
        }
        if (len == 0) {
            continue;
        }
        if (strcasecmp(input, "ETIRW") == 0) {
            Message done_msg = {0};
            (void)snprintf(done_msg.type, sizeof(done_msg.type), "%s", "WRITE_DONE");
            (void)snprintf(done_msg.id, sizeof(done_msg.id), "%ld", (long)time(NULL));
            (void)snprintf(done_msg.username, sizeof(done_msg.username), "%s", g_username);
            (void)snprintf(done_msg.role, sizeof(done_msg.role), "%s", "CLIENT");
            done_msg.payload[0] = '\0';
            if (proto_format_line(&done_msg, line_buf, sizeof(line_buf)) != 0 ||
                send_all(ss_fd, line_buf, strlen(line_buf)) != 0) {
                printf("Error: Failed to send ETIRW\n");
                fflush(stdout);
                close(ss_fd);
                return -1;
            }
            int n = recv_line(ss_fd, line_buf, sizeof(line_buf));
            if (n <= 0) {
                printf("Error: No response after ETIRW\n");
                fflush(stdout);
                close(ss_fd);
                return -1;
            }
            Message resp;
            if (proto_parse_line(line_buf, &resp) != 0) {
                printf("Error: Failed to parse response\n");
                fflush(stdout);
                close(ss_fd);
                return -1;
            }
            if (strcmp(resp.type, "ERROR") == 0) {
                char error_code[64];
                char error_msg[256];
                if (proto_parse_error(&resp, error_code, sizeof(error_code),
                                      error_msg, sizeof(error_msg)) == 0) {
                    printf("ERROR [%s]: %s\n", error_code, error_msg);
                } else {
                    printf("ERROR: %s\n", resp.payload);
                }
                fflush(stdout);
                close(ss_fd);
                return -1;
            }
            if (strcmp(resp.type, "ACK") == 0) {
                if (strlen(resp.payload) > 0) {
                    printf("%s\n", resp.payload);
                } else {
                    printf("Write Successful!\n");
                }
                fflush(stdout);
            }
            close(ss_fd);
            return 0;
        }

        char *space = strchr(input, ' ');
        if (!space) {
            printf("Error: Provide '<word_index> <content>'\n");
            fflush(stdout);
            continue;
        }
        *space = '\0';
        char *content = space + 1;
        while (*content == ' ') content++;
        if (*content == '\0') {
            printf("Error: Content cannot be empty\n");
            fflush(stdout);
            continue;
        }
        int word_index = atoi(input);
        Message edit_msg = {0};
        (void)snprintf(edit_msg.type, sizeof(edit_msg.type), "%s", "WRITE_EDIT");
        (void)snprintf(edit_msg.id, sizeof(edit_msg.id), "%ld", (long)time(NULL));
        (void)snprintf(edit_msg.username, sizeof(edit_msg.username), "%s", g_username);
        (void)snprintf(edit_msg.role, sizeof(edit_msg.role), "%s", "CLIENT");
        (void)snprintf(edit_msg.payload, sizeof(edit_msg.payload), "%d|%s", word_index, content);
        if (proto_format_line(&edit_msg, line_buf, sizeof(line_buf)) != 0 ||
            send_all(ss_fd, line_buf, strlen(line_buf)) != 0) {
            printf("Error: Failed to send edit\n");
            fflush(stdout);
            close(ss_fd);
            return -1;
        }
        int n = recv_line(ss_fd, line_buf, sizeof(line_buf));
        if (n <= 0) {
            printf("Error: Connection closed during WRITE\n");
            fflush(stdout);
            close(ss_fd);
            return -1;
        }
        Message resp;
        if (proto_parse_line(line_buf, &resp) != 0) {
            printf("Error: Failed to parse response\n");
            fflush(stdout);
            close(ss_fd);
            return -1;
        }
        if (strcmp(resp.type, "ERROR") == 0) {
            char error_code[64];
            char error_msg[256];
            if (proto_parse_error(&resp, error_code, sizeof(error_code),
                                  error_msg, sizeof(error_msg)) == 0) {
                printf("ERROR [%s]: %s\n", error_code, error_msg);
            } else {
                printf("ERROR: %s\n", resp.payload);
            }
            fflush(stdout);
            continue;
        }
        if (strcmp(resp.type, "ACK") == 0 && strlen(resp.payload) > 0) {
            printf("%s\n", resp.payload);
            fflush(stdout);
        }
    }
    close(ss_fd);
    return 0;
}

static int perform_undo_command(const ParsedCommand *cmd, const char *ss_host, int ss_port) {
    if (!cmd || cmd->argc < 1) {
        printf("Error: UNDO requires a filename\n");
        fflush(stdout);
        return -1;
    }
    int ss_fd = connect_to_host(ss_host, ss_port);
    if (ss_fd < 0) {
        printf("Error: Failed to connect to storage server at %s:%d\n", ss_host, ss_port);
        fflush(stdout);
        return -1;
    }
    Message req = {0};
    (void)snprintf(req.type, sizeof(req.type), "%s", "UNDO");
    (void)snprintf(req.id, sizeof(req.id), "%ld", (long)time(NULL));
    (void)snprintf(req.username, sizeof(req.username), "%s", g_username);
    (void)snprintf(req.role, sizeof(req.role), "%s", "CLIENT");
    (void)snprintf(req.payload, sizeof(req.payload), "%s", cmd->args[0]);

    char line_buf[MAX_LINE];
    if (proto_format_line(&req, line_buf, sizeof(line_buf)) != 0 ||
        send_all(ss_fd, line_buf, strlen(line_buf)) != 0) {
        printf("Error: Failed to send UNDO request to SS\n");
        fflush(stdout);
        close(ss_fd);
        return -1;
    }

    int n = recv_line(ss_fd, line_buf, sizeof(line_buf));
    if (n <= 0) {
        printf("Error: No response from storage server\n");
        fflush(stdout);
        close(ss_fd);
        return -1;
    }
    Message resp;
    if (proto_parse_line(line_buf, &resp) != 0) {
        printf("Error: Failed to parse response from SS\n");
        fflush(stdout);
        close(ss_fd);
        return -1;
    }
    if (strcmp(resp.type, "ERROR") == 0) {
        char error_code[64];
        char error_msg[256];
        if (proto_parse_error(&resp, error_code, sizeof(error_code),
                              error_msg, sizeof(error_msg)) == 0) {
            printf("ERROR [%s]: %s\n", error_code, error_msg);
        } else {
            printf("ERROR: %s\n", resp.payload);
        }
    } else if (strcmp(resp.type, "ACK") == 0) {
        if (strlen(resp.payload) > 0) {
            printf("%s\n", resp.payload);
        } else {
            printf("Undo Successful!\n");
        }
    } else {
        printf("Response: type=%s payload=%s\n", resp.type, resp.payload);
    }
    fflush(stdout);
    close(ss_fd);
    return 0;
}

// Interactive command loop
// Reads commands from stdin until EOF or EXIT command
static void command_loop(void) {
    char line[MAX_CMD_LINE];
    
    printf("LangOS Client - Type commands (or 'EXIT' to quit)\n");
    printf("> ");
    fflush(stdout);
    
    while (fgets(line, sizeof(line), stdin)) {
        // Parse command
        ParsedCommand cmd = parse_command(line);
        
        // Check for empty command
        if (strlen(cmd.cmd) == 0) {
            printf("> ");
            fflush(stdout);
            continue;
        }
        
        // Check for EXIT command
        if (strcmp(cmd.cmd, "EXIT") == 0) {
            printf("Exiting...\n");
            break;
        }

        if (strcmp(cmd.cmd, "HELP") == 0) {
            print_help_message();
            printf("> ");
            fflush(stdout);
            continue;
        }
        
        // Validate command arguments
        int valid = 1;
        if (strcmp(cmd.cmd, "CHECKPOINT") == 0) {
            if (cmd.argc < 2) {
                printf("ERROR: CHECKPOINT requires 2 arguments: <filename> <checkpoint_tag>\n");
                printf("Usage: CHECKPOINT <filename> <checkpoint_tag>\n");
                valid = 0;
            }
        } else if (strcmp(cmd.cmd, "VIEWCHECKPOINT") == 0) {
            if (cmd.argc < 2) {
                printf("ERROR: VIEWCHECKPOINT requires 2 arguments: <filename> <checkpoint_tag>\n");
                printf("Usage: VIEWCHECKPOINT <filename> <checkpoint_tag>\n");
                valid = 0;
            }
        } else if (strcmp(cmd.cmd, "REVERT") == 0) {
            if (cmd.argc < 2) {
                printf("ERROR: REVERT requires 2 arguments: <filename> <checkpoint_tag>\n");
                printf("Usage: REVERT <filename> <checkpoint_tag>\n");
                valid = 0;
            }
        } else if (strcmp(cmd.cmd, "LISTCHECKPOINTS") == 0) {
            if (cmd.argc < 1) {
                printf("ERROR: LISTCHECKPOINTS requires 1 argument: <filename>\n");
                printf("Usage: LISTCHECKPOINTS <filename>\n");
                valid = 0;
            }
        } else if (strcmp(cmd.cmd, "CREATE") == 0 || strcmp(cmd.cmd, "DELETE") == 0 || 
                   strcmp(cmd.cmd, "READ") == 0 || strcmp(cmd.cmd, "INFO") == 0 ||
                   strcmp(cmd.cmd, "STREAM") == 0 || strcmp(cmd.cmd, "UNDO") == 0 ||
                   strcmp(cmd.cmd, "EXEC") == 0 || strcmp(cmd.cmd, "CREATEFOLDER") == 0 ||
                   strcmp(cmd.cmd, "VIEWFOLDER") == 0) {
            if (cmd.argc < 1) {
                printf("ERROR: %s requires 1 argument: <filename>\n", cmd.cmd);
                valid = 0;
            }
        } else if (strcmp(cmd.cmd, "WRITE") == 0) {
            if (cmd.argc < 2) {
                printf("ERROR: WRITE requires 2 arguments: <filename> <sentence_number>\n");
                printf("Usage: WRITE <filename> <sentence_number>\n");
                valid = 0;
            }
        } else if (strcmp(cmd.cmd, "ADDACCESS") == 0) {
            if (cmd.argc < 2 || !cmd.has_flags) {
                printf("ERROR: ADDACCESS requires flag and 2 arguments: <filename> <username>\n");
                printf("Usage: ADDACCESS -R <filename> <username> or ADDACCESS -W <filename> <username>\n");
                valid = 0;
            }
        } else if (strcmp(cmd.cmd, "REMACCESS") == 0 || strcmp(cmd.cmd, "MOVE") == 0) {
            if (cmd.argc < 2) {
                printf("ERROR: %s requires 2 arguments\n", cmd.cmd);
                valid = 0;
            }
        }
        
        if (!valid) {
            printf("> ");
            fflush(stdout);
            continue;
        }
        
        // Send command and receive response
        send_command_and_receive(&cmd);
        
        printf("> ");
        fflush(stdout);
    }
}

int main(int argc, char **argv) {
    // Parse command line arguments
    const char *nm_host = "127.0.0.1";
    int nm_port = 5000;
    const char *username = "alice";
    
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--nm-host") && i+1 < argc) {
            nm_host = argv[++i];
        } else if (!strcmp(argv[i], "--nm-port") && i+1 < argc) {
            nm_port = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--username") && i+1 < argc) {
            username = argv[++i];
        }
    }
    
    // Store username
    strncpy(g_username, username, sizeof(g_username) - 1);
    
    // Connect to NM
    g_nm_fd = connect_to_host(nm_host, nm_port);
    if (g_nm_fd < 0) {
        perror("Error: Failed to connect to Name Server");
        return 1;
    }
    
    // Register with NM
    Message reg_msg = {0};
    (void)snprintf(reg_msg.type, sizeof(reg_msg.type), "%s", "CLIENT_REGISTER");
    (void)snprintf(reg_msg.id, sizeof(reg_msg.id), "%s", "1");
    (void)snprintf(reg_msg.username, sizeof(reg_msg.username), "%s", username);
    (void)snprintf(reg_msg.role, sizeof(reg_msg.role), "%s", "CLIENT");
    reg_msg.payload[0] = '\0';
    
    char reg_line[MAX_LINE];
    proto_format_line(&reg_msg, reg_line, sizeof(reg_line));
    send_all(g_nm_fd, reg_line, strlen(reg_line));
    
    // Wait for ACK
    char ack_buf[MAX_LINE];
    if (recv_line(g_nm_fd, ack_buf, sizeof(ack_buf)) > 0) {
        Message ack;
        if (proto_parse_line(ack_buf, &ack) == 0) {
            log_info("client_registered", "user=%s", username);
        }
    }
    
    // Enter interactive command loop
    command_loop();
    
    // Cleanup
    close(g_nm_fd);
    return 0;
}
