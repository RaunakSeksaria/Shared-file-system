#ifndef PROTOCOL_H
#define PROTOCOL_H

// Protocol definitions for our simple line-based wire format.
// Each TCP line is a single message with fields separated by '|':
//   TYPE|ID|USERNAME|ROLE|PAYLOAD\n
// This header defines the message structure and helpers to parse/format
// a message safely into fixed-size buffers.

#include <stddef.h>

// Message Types:
//   Registration: SS_REGISTER, CLIENT_REGISTER
//   Status: HEARTBEAT, ACK, ERROR
//   File Operations: CREATE, DELETE, READ, WRITE, STREAM, INFO, UNDO, EXEC
//   User Operations: VIEW, LIST, ADDACCESS, REMACCESS
//   Folder Operations: CREATE_FOLDER/CREATEFOLDER, MOVE, VIEWFOLDER/VIEW_FOLDER
//   Internal: DATA, STOP, GET_FILE, GET_ACL, UPDATE_ACL

#define MAX_LINE 2048

typedef struct Message {
    // All fields are null-terminated strings.
    char type[32];
    char id[64];
    char username[64];
    char role[16];
    char payload[1792];
} Message;

int proto_parse_line(const char *line, Message *out);
int proto_format_line(const Message *msg, char *buf, size_t buflen);

// Helper functions for error messages
// Format an error response message
// Error info is embedded in payload as: "ERROR_CODE|ERROR_MESSAGE"
int proto_format_error(const char *id, const char *username, const char *role, 
                       const char *error_code, const char *error_msg, 
                       char *buf, size_t buflen);

// Parse error from message payload (extracts ERROR_CODE|ERROR_MESSAGE)
// Returns 0 on success, -1 if payload doesn't contain error format
int proto_parse_error(const Message *msg, char *error_code, size_t code_sz, 
                      char *error_msg, size_t msg_sz);

#endif

