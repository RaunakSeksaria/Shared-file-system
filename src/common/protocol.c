#include "protocol.h"

// Parser/formatter for the line-based protocol.
// We split by '|' for the first four fields and assign the remainder to payload.

#include <stdio.h>
#include <string.h>

// Copy helper that always null-terminates.
static void safe_copy(char *dst, size_t dstsz, const char *src) {
    if (!dst || dstsz == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    // Use snprintf to avoid strncpy truncation warnings.
    (void)snprintf(dst, dstsz, "%s", src);
}

int proto_parse_line(const char *line, Message *out) {
    if (!line || !out) return -1;
    char tmp[MAX_LINE];
    safe_copy(tmp, sizeof(tmp), line);
    // Strip one trailing newline or carriage return if present.
    size_t len = strlen(tmp);
    if (len > 0 && (tmp[len-1] == '\n' || tmp[len-1] == '\r')) {
        tmp[--len] = '\0';
    }
    
    // Parse manually to handle empty fields correctly
    // Format: TYPE|ID|USERNAME|ROLE|PAYLOAD
    // Find the positions of the 4 separators
    char *p = tmp;
    char *fields[5];  // type, id, username, role, payload
    
    fields[0] = p;  // Start of type
    
    // Find each field separator
    for (int i = 0; i < 4 && *p; i++) {
        // Find next '|'
        while (*p && *p != '|') p++;
        if (*p == '|') {
            *p = '\0';  // Null-terminate current field
            p++;  // Move past '|'
            fields[i + 1] = p;  // Start of next field
        } else {
            // No more separators found
            return -1;
        }
    }
    
    // fields[4] is the payload (everything after the 4th '|')
    // Copy each field
    safe_copy(out->type, sizeof(out->type), fields[0]);
    safe_copy(out->id, sizeof(out->id), fields[1]);
    safe_copy(out->username, sizeof(out->username), fields[2]);
    safe_copy(out->role, sizeof(out->role), fields[3]);
    safe_copy(out->payload, sizeof(out->payload), fields[4]);
    
    return 0;
}

int proto_format_line(const Message *msg, char *buf, size_t buflen) {
    if (!msg || !buf || buflen == 0) return -1;
    int n = snprintf(buf, buflen, "%s|%s|%s|%s|%s\n",
        msg->type, msg->id, msg->username, msg->role, msg->payload);
    return (n < 0 || (size_t)n >= buflen) ? -1 : 0;
}

// Format an error message: ERROR|ID|USERNAME|ROLE|ERROR_CODE|ERROR_MESSAGE
// This creates a structured error response that can be parsed by the receiver
int proto_format_error(const char *id, const char *username, const char *role,
                       const char *error_code, const char *error_msg,
                       char *buf, size_t buflen) {
    if (!buf || buflen == 0) return -1;
    
    // Build payload as: ERROR_CODE|ERROR_MESSAGE
    char payload[512];
    int n = snprintf(payload, sizeof(payload), "%s|%s", 
                     error_code ? error_code : "UNKNOWN",
                     error_msg ? error_msg : "");
    if (n < 0 || (size_t)n >= sizeof(payload)) return -1;
    
    // Format full message line
    n = snprintf(buf, buflen, "ERROR|%s|%s|%s|%s\n",
                 id ? id : "",
                 username ? username : "",
                 role ? role : "",
                 payload);
    return (n < 0 || (size_t)n >= buflen) ? -1 : 0;
}

// Parse error information from message payload
// Expected format: ERROR_CODE|ERROR_MESSAGE
// Extracts both parts into separate buffers
int proto_parse_error(const Message *msg, char *error_code, size_t code_sz,
                      char *error_msg, size_t msg_sz) {
    if (!msg || !error_code || !error_msg || code_sz == 0 || msg_sz == 0) return -1;
    
    // Copy payload to temporary buffer for parsing
    char tmp[512];
    strncpy(tmp, msg->payload, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    
    // Split by '|' to get ERROR_CODE and ERROR_MESSAGE
    char *saveptr = NULL;
    char *tok = strtok_r(tmp, "|", &saveptr);
    if (!tok) return -1;  // No error code found
    
    // Copy error code
    strncpy(error_code, tok, code_sz - 1);
    error_code[code_sz - 1] = '\0';
    
    // Get error message (rest of the line)
    tok = strtok_r(NULL, "", &saveptr);
    if (!tok) tok = "";  // No error message, use empty string
    
    // Copy error message
    strncpy(error_msg, tok, msg_sz - 1);
    error_msg[msg_sz - 1] = '\0';
    
    return 0;
}


