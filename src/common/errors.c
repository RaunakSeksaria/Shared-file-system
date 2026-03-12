#include "errors.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// Convert error code enum to human-readable string
// This is used in logging and protocol messages
const char *error_code_to_string(ErrorCode code) {
    switch (code) {
        case ERR_OK:          return "OK";
        case ERR_INVALID:     return "INVALID";
        case ERR_UNAUTHORIZED: return "UNAUTHORIZED";
        case ERR_NOT_FOUND:   return "NOT_FOUND";
        case ERR_CONFLICT:    return "CONFLICT";
        case ERR_UNAVAILABLE: return "UNAVAILABLE";
        case ERR_INTERNAL:    return "INTERNAL";
        default:              return "UNKNOWN";
    }
}

// Create an error with a formatted message (supports printf-style formatting)
// This allows us to create detailed error messages like: "File 'test.txt' not found"
Error error_create(ErrorCode code, const char *fmt, ...) {
    Error err;
    err.code = code;
    
    // Use va_list to handle variable arguments (like printf)
    va_list args;
    va_start(args, fmt);
    vsnprintf(err.message, sizeof(err.message), fmt, args);
    va_end(args);
    
    // Ensure null termination
    err.message[sizeof(err.message) - 1] = '\0';
    
    return err;
}

// Create an error with a simple string message (no formatting)
// Simpler version when you don't need formatted messages
Error error_simple(ErrorCode code, const char *message) {
    Error err;
    err.code = code;
    
    // Copy message safely, ensuring null termination
    if (message) {
        strncpy(err.message, message, sizeof(err.message) - 1);
        err.message[sizeof(err.message) - 1] = '\0';
    } else {
        err.message[0] = '\0';
    }
    
    return err;
}

// Check if error indicates success (convenience function)
int error_is_ok(const Error *err) {
    if (!err) return 0;  // NULL error is not OK
    return err->code == ERR_OK;
}

