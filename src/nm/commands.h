#ifndef COMMANDS_H
#define COMMANDS_H

#include "../common/protocol.h"
#include "../common/errors.h"
#include "index.h"
#include "access_control.h"

// Command handlers for Name Server
// Process client commands and return responses

// Handle VIEW command
// client_fd: File descriptor to send response to
// username: Username of requesting client
// flags: Flags string (e.g., "al" for -al)
// Returns: 0 on success, -1 on error
//
// VIEW command: Lists files user has access to
// VIEW -a: Lists all files on system
// VIEW -l: Lists files with details
// VIEW -al: Lists all files with details
//
// This function:
// 1. Gets files from index (all or user's files based on -a flag)
// 2. Filters by access if no -a flag (check ACL for each file)
// 3. Formats output with details if -l flag
// 4. Sends formatted response to client
int handle_view(int client_fd, const char *username, const char *flags);

// Handle CREATE command
// client_fd: File descriptor to send response to
// username: Username of requesting client
// filename: Name of file to create
// Returns: 0 on success, -1 on error
//
// CREATE command: Creates a new empty file
//
// This function:
// 1. Checks if file already exists in index (return CONFLICT if yes)
// 2. Selects appropriate SS (round-robin for now)
// 3. Sends CREATE command to SS
// 4. Waits for ACK from SS
// 5. Adds file to index with owner = requester
// 6. Sends success response to client
int handle_create(int client_fd, const char *username, const char *filename);

// Handle DELETE command
// client_fd: File descriptor to send response to
// username: Username of requesting client
// filename: Name of file to delete
// Returns: 0 on success, -1 on error
//
// DELETE command: Deletes a file (owner only)
//
// This function:
// 1. Looks up file in index (return NOT_FOUND if not exists)
// 2. Checks if user is owner (return UNAUTHORIZED if not)
// 3. Finds SS hosting the file
// 4. Sends DELETE command to SS
// 5. Waits for ACK from SS
// 6. Removes file from index
// 7. Sends success response to client
int handle_delete(int client_fd, const char *username, const char *filename);

// Handle INFO command
// client_fd: File descriptor to send response to
// username: Username of requesting client
// filename: Name of file to get info for
// Returns: 0 on success, -1 on error
//
// INFO command: Displays file metadata
//
// This function:
// 1. Looks up file in index (return NOT_FOUND if not exists)
// 2. Checks read access (return UNAUTHORIZED if no access)
// 3. Updates last_accessed timestamp
// 4. Formats metadata (owner, timestamps, size, counts, ACL)
// 5. Sends formatted response to client
int handle_info(int client_fd, const char *username, const char *filename);

// Handle LIST command
// client_fd: File descriptor to send response to
// username: Username of requesting client (not used, but kept for consistency)
// Returns: 0 on success, -1 on error
//
// LIST command: Lists all registered users
//
// This function:
// 1. Gets all clients from registry
// 2. Formats as list
// 3. Sends response to client
int handle_list(int client_fd, const char *username);

// Handle READ command
// client_fd: File descriptor to send response to
// username: Username of requesting client
// filename: Name of file to read
// Returns: 0 on success, -1 on error
//
// READ command: Returns SS connection info for client to connect directly
//
// This function:
// 1. Looks up file in index (return NOT_FOUND if not exists)
// 2. Checks read access (return UNAUTHORIZED if no access)
// 3. Gets SS host and port from file entry
// 4. Sends SS_INFO message with host=IP,port=PORT
int handle_read(int client_fd, const char *username, const char *filename);

// Handle STREAM command
// client_fd: File descriptor to send response to
// username: Username of requesting client
// filename: Name of file to stream
// Returns: 0 on success, -1 on error
//
// STREAM command: Returns SS connection info for client to connect directly
//
// This function:
// 1. Looks up file in index (return NOT_FOUND if not exists)
// 2. Checks read access (return UNAUTHORIZED if no access)
// 3. Gets SS host and port from file entry
// 4. Sends SS_INFO message with host=IP,port=PORT
int handle_stream(int client_fd, const char *username, const char *filename);

int handle_undo(int client_fd, const char *username, const char *filename);
int handle_exec(int client_fd, const char *username, const char *filename, const char *request_id);

// Handle WRITE command - returns SS info after checking write access
int handle_write(int client_fd, const char *username, const char *filename, int sentence_index);

// Handle checkpoint commands
int handle_checkpoint(int client_fd, const char *username, const char *filename, const char *tag);
int handle_viewcheckpoint(int client_fd, const char *username, const char *filename, const char *tag);
int handle_revert_checkpoint(int client_fd, const char *username, const char *filename, const char *tag);
int handle_listcheckpoints(int client_fd, const char *username, const char *filename);

// Handle ADDACCESS command
// client_fd: File descriptor to send response to
// username: Username of requesting client (must be file owner)
// flag: "R" for read access, "W" for write access
// filename: Name of file
// target_username: Username to grant access to
// Returns: 0 on success, -1 on error
//
// ADDACCESS command: Grants read or write access to a user
//
// This function:
// 1. Looks up file in index (return NOT_FOUND if not exists)
// 2. Verifies requester is file owner (return UNAUTHORIZED if not)
// 3. Connects to SS and sends UPDATE_ACL command
// 4. Waits for ACK from SS
// 5. Sends success response to client
int handle_addaccess(int client_fd, const char *username, const char *flag,
                     const char *filename, const char *target_username);

// Handle REMACCESS command
// client_fd: File descriptor to send response to
// username: Username of requesting client (must be file owner)
// filename: Name of file
// target_username: Username to remove access from
// Returns: 0 on success, -1 on error
//
// REMACCESS command: Removes all access from a user
//
// This function:
// 1. Looks up file in index (return NOT_FOUND if not exists)
// 2. Verifies requester is file owner (return UNAUTHORIZED if not)
// 3. Connects to SS and sends UPDATE_ACL command
// 4. Waits for ACK from SS
// 5. Sends success response to client
int handle_remaccess(int client_fd, const char *username,
                     const char *filename, const char *target_username);

// Helper: Send error response to client
// client_fd: File descriptor
// id: Message ID
// username: Username
// error: Error structure
// Returns: 0 on success, -1 on error
int send_error_response(int client_fd, const char *id, const char *username,
                       const Error *error);

// Helper: Send success response to client
// client_fd: File descriptor
// id: Message ID
// username: Username
// message: Success message
// Returns: 0 on success, -1 on error
int send_success_response(int client_fd, const char *id, const char *username,
                          const char *message);

// Helper: Send data response to client
// client_fd: File descriptor
// id: Message ID
// username: Username
// data: Data to send (formatted output)
// Returns: 0 on success, -1 on error
int send_data_response(int client_fd, const char *id, const char *username,
                      const char *data);

// ===== Folder Command Handlers =====

// Handle CREATE_FOLDER command
// client_fd: File descriptor to send response to
// username: Username of requesting client
// folder_path: Path of folder to create (e.g., "/folder1/subfolder/")
// Returns: 0 on success, -1 on error
//
// CREATE_FOLDER command: Creates a new folder
//
// This function:
// 1. Validates folder path (must start with /, no .., etc.)
// 2. Checks if folder already exists (return CONFLICT if yes)
// 3. Selects appropriate SS
// 4. Sends CREATE_FOLDER command to SS
// 5. Waits for ACK from SS
// 6. Adds folder to index
// 7. Sends success response to client
int handle_createfolder(int client_fd, const char *username, const char *folder_path);

// Handle MOVE command
// client_fd: File descriptor to send response to
// username: Username of requesting client
// filename: Name of file to move (with current folder path)
// new_folder_path: Destination folder path
// Returns: 0 on success, -1 on error
//
// MOVE command: Moves a file from one folder to another
//
// This function:
// 1. Looks up file in index (return NOT_FOUND if not exists)
// 2. Checks if user has write access (return UNAUTHORIZED if not)
// 3. Validates destination folder exists
// 4. Sends MOVE command to SS
// 5. Waits for ACK from SS
// 6. Updates file's folder_path in index
// 7. Sends success response to client
int handle_move(int client_fd, const char *username, const char *filename,
                const char *new_folder_path);

// Handle VIEWFOLDER command
// client_fd: File descriptor to send response to
// username: Username of requesting client
// folder_path: Path of folder to view
// Returns: 0 on success, -1 on error
//
// VIEWFOLDER command: Lists contents of a folder
//
// This function:
// 1. Checks if folder exists (return NOT_FOUND if not)
// 2. Gets all files in folder from index
// 3. Gets all subfolders in folder from index
// 4. Formats output with files and folders
// 5. Sends response to client
int handle_viewfolder(int client_fd, const char *username, const char *folder_path);

// Handle REQUESTACCESS command (alias: RACC)
// client_fd: File descriptor to send response to
// username: Username of requesting client
// payload: "filename|access_type" (e.g., "/documents/report.txt|R")
// Returns: 0 on success, -1 on error
//
// REQUESTACCESS command: Request read/write access to a file
//
// This function:
// 1. Parses filename and access type (R/W/RW)
// 2. Looks up file (return NOT_FOUND if not exists)
// 3. Checks user is not owner (return INVALID if owner)
// 4. Fetches ACL and checks if user already has requested access
// 5. Checks for duplicate pending request
// 6. Adds request to queue
// 7. Returns request ID to user
int handle_requestaccess(int client_fd, const char *username, const char *payload);

// Handle VIEWACCESSREQUESTS command (alias: VIEWACCR)
// client_fd: File descriptor to send response to
// username: Username of requesting client (file owner)
// payload: Optional filename filter (empty for all requests)
// Returns: 0 on success, -1 on error
//
// VIEWACCESSREQUESTS command: View pending access requests for your files
//
// This function:
// 1. Optionally parses filename filter
// 2. Gets all requests for files owned by user
// 3. Formats as table with ID, requester, file, type, date
// 4. Sends response to client
int handle_viewaccessrequests(int client_fd, const char *username, const char *payload);

// Handle APPROVEACCESSREQUEST command (alias: APPROVEACCR)
// client_fd: File descriptor to send response to
// username: Username of requesting client (file owner)
// payload: Request ID
// Returns: 0 on success, -1 on error
//
// APPROVEACCESSREQUEST command: Approve pending access request
//
// This function:
// 1. Parses request ID
// 2. Gets request from queue (return NOT_FOUND if not exists)
// 3. Verifies user is owner (return UNAUTHORIZED if not)
// 4. Looks up file (return NOT_FOUND if deleted)
// 5. Sends ADDACCESS command to SS
// 6. Removes request from queue on success
// 7. Sends success response to client
int handle_approveaccessrequest(int client_fd, const char *username, const char *payload);

// Handle DISAPPROVEACCESSREQUEST command (alias: DISACCR)
// client_fd: File descriptor to send response to
// username: Username of requesting client (file owner)
// payload: Request ID
// Returns: 0 on success, -1 on error
//
// DISAPPROVEACCESSREQUEST command: Deny pending access request
//
// This function:
// 1. Parses request ID
// 2. Gets request from queue (return NOT_FOUND if not exists)
// 3. Verifies user is owner (return UNAUTHORIZED if not)
// 4. Removes request from queue
// 5. Sends success response to client
int handle_disapproveaccessrequest(int client_fd, const char *username, const char *payload);

#endif

