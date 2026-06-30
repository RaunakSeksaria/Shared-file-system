#ifndef WRITE_SESSION_H
#define WRITE_SESSION_H

#include <stddef.h>
#include "sentence_parser.h"

#define WRITE_SESSION_MAX_FILENAME 256
#define WRITE_SESSION_MAX_PATH 512

typedef struct {
    char storage_dir[WRITE_SESSION_MAX_PATH];
    char filename[WRITE_SESSION_MAX_FILENAME];
    char username[64];
    int sentence_index;
    int sentence_id;
    int session_id;
    int active;
    SentenceEntry sentence_entry;
} WriteSession;

int write_session_begin(WriteSession *session,
                        const char *storage_dir,
                        const char *filename,
                        int sentence_index,
                        const char *username,
                        char **current_text_out,
                        char *error_buf, size_t error_buf_len);

int write_session_apply_edit(WriteSession *session,
                             int word_index,
                             const char *content,
                             char *error_buf, size_t error_buf_len);

char *write_session_get_current_text(const WriteSession *session);

int write_session_commit(WriteSession *session,
                         char *error_buf, size_t error_buf_len);

void write_session_abort(WriteSession *session);

#endif


