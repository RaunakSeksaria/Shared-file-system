#define _POSIX_C_SOURCE 200809L  // For strdup
#include "commands.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Parse a command line
ParsedCommand parse_command(const char *line) {
    ParsedCommand cmd = {0};
    
    if (!line || strlen(line) == 0) {
        return cmd;  // Empty command
    }
    
    // Make copy for tokenization
    char *line_copy = strdup(line);
    if (!line_copy) return cmd;
    
    // Remove trailing newline
    size_t len = strlen(line_copy);
    while (len > 0 && (line_copy[len-1] == '\n' || line_copy[len-1] == '\r')) {
        line_copy[--len] = '\0';
    }
    
    // Skip leading whitespace
    char *p = line_copy;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0') {
        free(line_copy);
        return cmd;
    }
    
    // Tokenize by spaces
    char *saveptr = NULL;
    char *tok = strtok_r(p, " \t", &saveptr);
    int arg_idx = 0;
    int args_count = 0;  // Separate counter for actual arguments
    
    while (tok && arg_idx < MAX_ARGS) {
        if (arg_idx == 0) {
            // First token is command name
            strncpy(cmd.cmd, tok, sizeof(cmd.cmd) - 1);
            // Convert to uppercase for consistency
            for (char *q = cmd.cmd; *q; q++) {
                if (*q >= 'a' && *q <= 'z') *q = *q - 'a' + 'A';
            }
        } else if (tok[0] == '-') {
            // Flag (e.g., -a, -l, -al)
            // Remove leading '-'
            const char *flag_str = tok + 1;
            // Add flags to flags string (e.g., "al" from "-al")
            size_t flag_len = strlen(flag_str);
            size_t current_flags_len = strlen(cmd.flags);
            if (current_flags_len + flag_len < sizeof(cmd.flags)) {
                strcat(cmd.flags, flag_str);
                cmd.has_flags = 1;
            }
        } else {
            // Regular argument (not a flag)
            if (args_count < MAX_ARGS) {
                strncpy(cmd.args[args_count], tok, sizeof(cmd.args[0]) - 1);
                args_count++;
                cmd.argc++;
            }
        }
        tok = strtok_r(NULL, " \t", &saveptr);
        arg_idx++;
    }
    
    free(line_copy);
    return cmd;
}

// Format command message for sending to NM
int format_command_message(const ParsedCommand *cmd, const char *username,
                          char *buf, size_t buflen) {
    if (!cmd || !username || !buf || buflen == 0) return -1;
    
    // Map aliases to full command names
    const char *message_type = cmd->cmd;
    if (strcmp(cmd->cmd, "RACC") == 0) {
        message_type = "REQUESTACCESS";
    } else if (strcmp(cmd->cmd, "VIEWACCR") == 0) {
        message_type = "VIEWACCESSREQUESTS";
    } else if (strcmp(cmd->cmd, "APPROVEACCR") == 0) {
        message_type = "APPROVEACCESSREQUEST";
    } else if (strcmp(cmd->cmd, "DISACCR") == 0) {
        message_type = "DISAPPROVEACCESSREQUEST";
    }
    
    // Build payload based on command type
    char payload[1024] = {0};
    
    // For VIEW command: flags=FLAGS (no arguments)
    if (strcmp(cmd->cmd, "VIEW") == 0) {
        if (cmd->has_flags && strlen(cmd->flags) > 0) {
            (void)snprintf(payload, sizeof(payload), "flags=%s", cmd->flags);
        } else {
            payload[0] = '\0';  // No flags
        }
    }
    // For ADDACCESS: flag|filename|username
    else if (strcmp(cmd->cmd, "ADDACCESS") == 0) {
        if (cmd->argc >= 2) {
            // Get flag from flags string (should be "R" or "W")
            const char *flag = (cmd->has_flags && strlen(cmd->flags) > 0) ? cmd->flags : "R";
            (void)snprintf(payload, sizeof(payload), "%s|%s|%s", flag, cmd->args[0], cmd->args[1]);
        } else {
            payload[0] = '\0';
        }
    }
    // For REMACCESS: filename|username
    else if (strcmp(cmd->cmd, "REMACCESS") == 0) {
        if (cmd->argc >= 2) {
            (void)snprintf(payload, sizeof(payload), "%s|%s", cmd->args[0], cmd->args[1]);
        } else {
            payload[0] = '\0';
        }
    }
    else if (strcmp(cmd->cmd, "WRITE") == 0) {
        if (cmd->argc >= 2) {
            (void)snprintf(payload, sizeof(payload), "%s|%s", cmd->args[0], cmd->args[1]);
        } else if (cmd->argc >= 1) {
            strncpy(payload, cmd->args[0], sizeof(payload) - 1);
        } else {
            payload[0] = '\0';
        }
    }
    // For MOVE: filename|new_folder_path
    else if (strcmp(cmd->cmd, "MOVE") == 0) {
        if (cmd->argc >= 2) {
            (void)snprintf(payload, sizeof(payload), "%s|%s", cmd->args[0], cmd->args[1]);
        } else {
            payload[0] = '\0';
        }
    }
    // For CHECKPOINT: filename|tag
    else if (strcmp(cmd->cmd, "CHECKPOINT") == 0) {
        if (cmd->argc >= 2) {
            (void)snprintf(payload, sizeof(payload), "%s|%s", cmd->args[0], cmd->args[1]);
        } else {
            payload[0] = '\0';
        }
    }
    // For VIEWCHECKPOINT: filename|tag
    else if (strcmp(cmd->cmd, "VIEWCHECKPOINT") == 0) {
        if (cmd->argc >= 2) {
            (void)snprintf(payload, sizeof(payload), "%s|%s", cmd->args[0], cmd->args[1]);
        } else {
            payload[0] = '\0';
        }
    }
    // For REVERT: filename|tag
    else if (strcmp(cmd->cmd, "REVERT") == 0) {
        if (cmd->argc >= 2) {
            (void)snprintf(payload, sizeof(payload), "%s|%s", cmd->args[0], cmd->args[1]);
        } else {
            payload[0] = '\0';
        }
    }
    // For LISTCHECKPOINTS: filename
    else if (strcmp(cmd->cmd, "LISTCHECKPOINTS") == 0) {
        if (cmd->argc >= 1) {
            strncpy(payload, cmd->args[0], sizeof(payload) - 1);
        } else {
            payload[0] = '\0';
        }
    }
    // For CREATE_FOLDER, CREATEFOLDER, VIEWFOLDER, VIEW_FOLDER
    else if (strcmp(cmd->cmd, "CREATEFOLDER") == 0 || 
             strcmp(cmd->cmd, "CREATE_FOLDER") == 0 ||
             strcmp(cmd->cmd, "VIEWFOLDER") == 0 ||
             strcmp(cmd->cmd, "VIEW_FOLDER") == 0) {
        if (cmd->argc >= 1) {
            strncpy(payload, cmd->args[0], sizeof(payload) - 1);
        } else {
            payload[0] = '\0';
        }
    }
    // For REQUESTACCESS / RACC: filename|access_type
    else if (strcmp(message_type, "REQUESTACCESS") == 0) {
        if (cmd->argc >= 2) {
            (void)snprintf(payload, sizeof(payload), "%s|%s", cmd->args[0], cmd->args[1]);
        } else {
            payload[0] = '\0';
        }
    }
    // For VIEWACCESSREQUESTS / VIEWACCR: optional filename filter
    else if (strcmp(message_type, "VIEWACCESSREQUESTS") == 0) {
        if (cmd->argc >= 1) {
            strncpy(payload, cmd->args[0], sizeof(payload) - 1);
        } else {
            payload[0] = '\0';  // No filter = all requests
        }
    }
    // For APPROVEACCESSREQUEST / APPROVEACCR: request_id
    else if (strcmp(message_type, "APPROVEACCESSREQUEST") == 0) {
        // printf("DEBUG: reached here\n");
        if (cmd->argc >= 1) {
            strncpy(payload, cmd->args[0], sizeof(payload) - 1);
        } else {
            payload[0] = '\0';
        }
    }
    // For DISAPPROVEACCESSREQUEST / DISACCR: request_id
    else if (strcmp(message_type, "DISAPPROVEACCESSREQUEST") == 0) {
        if (cmd->argc >= 1) {
            strncpy(payload, cmd->args[0], sizeof(payload) - 1);
        } else {
            payload[0] = '\0';
        }
    }
    // For commands with arguments (CREATE, DELETE, INFO, READ, STREAM, UNDO, EXEC, etc.)
    else if (cmd->argc > 0) {
        // First argument is the filename (or whatever the command needs)
        strncpy(payload, cmd->args[0], sizeof(payload) - 1);
    }
    // For LIST: empty payload
    else {
        payload[0] = '\0';
    }
    
    // Format message
    Message msg = {0};
    size_t cmd_len = strlen(message_type);
    size_t copy_len = (cmd_len < sizeof(msg.type) - 1) ? cmd_len : sizeof(msg.type) - 1;
    memcpy(msg.type, message_type, copy_len);
    msg.type[copy_len] = '\0';
    
    (void)snprintf(msg.id, sizeof(msg.id), "%ld", (long)time(NULL));  // Use timestamp as ID
    
    size_t username_len = strlen(username);
    copy_len = (username_len < sizeof(msg.username) - 1) ? username_len : sizeof(msg.username) - 1;
    memcpy(msg.username, username, copy_len);
    msg.username[copy_len] = '\0';
    
    memcpy(msg.role, "CLIENT", 6);
    msg.role[6] = '\0';
    
    size_t payload_len = strlen(payload);
    copy_len = (payload_len < sizeof(msg.payload) - 1) ? payload_len : sizeof(msg.payload) - 1;
    memcpy(msg.payload, payload, copy_len);
    msg.payload[copy_len] = '\0';
    
    return proto_format_line(&msg, buf, buflen);
}

