#ifndef ERRORS_H
#define ERRORS_H

// Error Code System
// Defines universal error codes used throughout the system (NM, SS, Client)
// These codes ensure consistent error handling and reporting across all components

// Error code constants - these match the requirements specification
typedef enum {
    ERR_OK = 0,              // Operation successful
    ERR_INVALID,             // Invalid request/parameters
    ERR_UNAUTHORIZED,        // User lacks required permissions
    ERR_NOT_FOUND,           // File/user/resource not found
    ERR_CONFLICT,            // Resource contention (e.g., file locked, already exists)
    ERR_UNAVAILABLE,         // Resource temporarily unavailable
    ERR_INTERNAL             // Internal server error
} ErrorCode;

// Error structure - contains both code and human-readable message
typedef struct {
    ErrorCode code;
    char message[256];  // Detailed error message for user
} Error;

// Convert error code to string (for logging and protocol)
// Returns static string, so don't free it
const char *error_code_to_string(ErrorCode code);

// Create an error with a formatted message (like printf)
// Usage: Error err = error_create(ERR_NOT_FOUND, "File '%s' not found", filename);
Error error_create(ErrorCode code, const char *fmt, ...);

// Create an error with a simple string message
// Usage: Error err = error_simple(ERR_OK, "Success");
Error error_simple(ErrorCode code, const char *message);

// Check if error indicates success (ERR_OK)
// Returns 1 if success, 0 if error
int error_is_ok(const Error *err);

// Helper to create success (no error)
// Returns error with ERR_OK code
static inline Error error_ok(void) {
    Error e = {ERR_OK, ""};
    return e;
}

#endif

