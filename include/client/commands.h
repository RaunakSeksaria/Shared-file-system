#ifndef COMMANDS_H
#define COMMANDS_H

#include <stddef.h>
#include "../common/protocol.h"

// Client command parsing and handling
// Parses user input and formats commands for sending to NM

// Maximum command length
#define MAX_CMD_LINE 1024
#define MAX_ARGS 32

// Command structure: parsed command with arguments
typedef struct {
    char cmd[32];           // Command name (VIEW, CREATE, READ, etc.)
    char args[MAX_ARGS][256];  // Command arguments
    int argc;               // Number of arguments
    int has_flags;          // Whether command has flags (like -a, -l)
    char flags[16];         // Flags string (e.g., "al" for -al)
} ParsedCommand;

// Parse a command line into ParsedCommand structure
// line: Input line from user (e.g., "VIEW -al" or "CREATE test.txt")
// Returns: ParsedCommand structure, or NULL on parse error
//
// This function:
// 1. Splits line by spaces
// 2. Extracts command name (first token)
// 3. Extracts flags (tokens starting with '-')
// 4. Extracts arguments (remaining tokens)
//
// Example:
//   "VIEW -al" -> cmd="VIEW", flags="al", argc=0
//   "CREATE test.txt" -> cmd="CREATE", args[0]="test.txt", argc=1
ParsedCommand parse_command(const char *line);

// Format command for sending to NM
// cmd: Parsed command structure
// username: Username of client
// buf: Buffer to write formatted command
// buflen: Size of buffer
// Returns: 0 on success, -1 on error
//
// Format: COMMAND|ID|USERNAME|CLIENT|PAYLOAD
// Payload contains command arguments and flags
int format_command_message(const ParsedCommand *cmd, const char *username,
                          char *buf, size_t buflen);

#endif

