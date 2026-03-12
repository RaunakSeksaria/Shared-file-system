#ifndef FILE_SCAN_H
#define FILE_SCAN_H

#include <stddef.h>

// File scanning module for Storage Server
// Scans storage directory on startup to discover existing files
// Builds file list for registration with Name Server

// Maximum number of files a single SS can track (can be increased if needed)
#define MAX_FILES_PER_SS 1000

// Structure to hold information about a single file discovered during scanning
typedef struct {
    char filename[256];      // Name of the file
    size_t size_bytes;       // File size in bytes
    int has_metadata;        // Whether metadata file exists (1) or not (0)
} ScannedFile;

// Structure to hold the result of a directory scan
typedef struct {
    ScannedFile files[MAX_FILES_PER_SS];  // Array of discovered files
    int count;                              // Number of files found
} ScanResult;

// Scan the storage directory for existing files
// storage_dir: Path to the storage directory (e.g., "./storage_ss1")
// files_dir: Subdirectory containing actual files (e.g., "files")
// Returns: ScanResult with list of discovered files
// 
// This function:
// 1. Opens the files directory
// 2. Reads each entry (file)
// 3. Gets file size
// 4. Checks if metadata exists
// 5. Populates ScanResult structure
//
// Usage:
//   ScanResult result = scan_directory("./storage_ss1", "files");
//   for (int i = 0; i < result.count; i++) {
//       printf("Found: %s (%zu bytes)\n", result.files[i].filename, result.files[i].size_bytes);
//   }
ScanResult scan_directory(const char *storage_dir, const char *files_dir);

// Build a file list string from ScanResult for sending to NM
// Format: "file1.txt,file2.txt,file3.txt" (comma-separated)
// This string is included in SS_REGISTER payload
//
// result: The scan result containing discovered files
// buf: Buffer to write the file list string into
// buflen: Size of the buffer
// Returns: 0 on success, -1 on error (buffer too small)
//
// Usage:
//   char file_list[4096];
//   build_file_list_string(&result, file_list, sizeof(file_list));
//   // file_list now contains: "file1.txt,file2.txt,..."
int build_file_list_string(const ScanResult *result, const char *storage_dir,
                           char *buf, size_t buflen);

#endif

