#define _POSIX_C_SOURCE 200809L
#include "write_session.h"

#include <ctype.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../common/log.h"
#include "file_storage.h"
#include "runtime_state.h"

typedef struct {
    char **items;
    size_t count;
} TokenList;

static void sentence_entry_clear(SentenceEntry *entry) {
    if (!entry || !entry->words) return;
    for (size_t i = 0; i < entry->word_count; i++) {
        free(entry->words[i].text);
    }
    free(entry->words);
    entry->words = NULL;
    entry->word_count = 0;
}

static int sentence_entry_clone(const SentenceEntry *src, SentenceEntry *dst) {
    if (!src || !dst) return -1;
    memset(dst, 0, sizeof(*dst));
    if (src->word_count == 0) {
        dst->sentence_id = src->sentence_id;
        dst->version = src->version;
        return 0;
    }
    SentenceWord *words = calloc(src->word_count, sizeof(SentenceWord));
    if (!words) return -1;
    for (size_t i = 0; i < src->word_count; i++) {
        if (!src->words[i].text) continue;
        words[i].text = strdup(src->words[i].text);
        if (!words[i].text) {
            for (size_t j = 0; j < i; j++) {
                free(words[j].text);
            }
            free(words);
            return -1;
        }
    }
    dst->words = words;
    dst->word_count = src->word_count;
    dst->sentence_id = src->sentence_id;
    dst->version = src->version;
    return 0;
}

static void free_tokens(TokenList *list) {
    if (!list || !list->items) return;
    for (size_t i = 0; i < list->count; i++) {
        free(list->items[i]);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
}

static int split_content_into_tokens(const char *content, TokenList *out) {
    if (!content || !out) return -1;
    memset(out, 0, sizeof(*out));
    const char *p = content;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        const char *start = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        size_t len = (size_t)(p - start);
        if (len == 0) continue;
        char *token = strndup(start, len);
        if (!token) {
            free_tokens(out);
            return -1;
        }
        char **new_items = realloc(out->items, sizeof(char*) * (out->count + 1));
        if (!new_items) {
            free(token);
            free_tokens(out);
            return -1;
        }
        out->items = new_items;
        out->items[out->count++] = token;
    }
    if (out->count == 0) {
        free_tokens(out);
        return -1;
    }
    return 0;
}

static int sentence_entry_insert_tokens(SentenceEntry *entry, size_t index,
                                        TokenList *tokens) {
    if (!entry || !tokens || index > entry->word_count) return -1;
    size_t new_count = entry->word_count + tokens->count;
    SentenceWord *words = realloc(entry->words, sizeof(SentenceWord) * new_count);
    if (!words) return -1;
    entry->words = words;
    if (index < entry->word_count) {
        memmove(&entry->words[index + tokens->count],
                &entry->words[index],
                sizeof(SentenceWord) * (entry->word_count - index));
    }
    for (size_t i = 0; i < tokens->count; i++) {
        entry->words[index + i].text = tokens->items[i];
        tokens->items[i] = NULL;
    }
    entry->word_count = new_count;
    tokens->count = 0;
    return 0;
}

static int assign_sentence_ids(SentenceCollection *collection, FileMetadata *metadata) {
    if (!collection || !metadata) return -1;
    int next_id = metadata->next_sentence_id;
    for (size_t i = 0; i < collection->count; i++) {
        if ((int)i < metadata->sentence_count && metadata->sentences[i].sentence_id > 0) {
            collection->sentences[i].sentence_id = metadata->sentences[i].sentence_id;
            collection->sentences[i].version = metadata->sentences[i].version;
        } else {
            collection->sentences[i].sentence_id = next_id++;
            collection->sentences[i].version = 1;
        }
    }
    metadata->next_sentence_id = next_id;
    return 0;
}

static char *sentence_entry_to_string(const SentenceEntry *entry) {
    if (!entry) return NULL;
    size_t total = 0;
    for (size_t i = 0; i < entry->word_count; i++) {
        if (i > 0) total++;
        if (entry->words[i].text) total += strlen(entry->words[i].text);
    }
    char *buffer = malloc(total + 1);
    if (!buffer) return NULL;
    size_t pos = 0;
    for (size_t i = 0; i < entry->word_count; i++) {
        if (i > 0) buffer[pos++] = ' ';
        if (entry->words[i].text) {
            size_t len = strlen(entry->words[i].text);
            memcpy(buffer + pos, entry->words[i].text, len);
            pos += len;
        }
    }
    buffer[pos] = '\0';
    return buffer;
}

static int render_collection(const SentenceCollection *collection,
                             char **out_text, size_t *out_len,
                             size_t *offsets, size_t *lengths) {
    if (!collection || !out_text || !offsets || !lengths) return -1;
    size_t total = 0;
    for (size_t i = 0; i < collection->count; i++) {
        size_t sentence_len = 0;
        for (size_t w = 0; w < collection->sentences[i].word_count; w++) {
            if (w > 0) sentence_len++;
            if (collection->sentences[i].words[w].text) {
                sentence_len += strlen(collection->sentences[i].words[w].text);
            }
        }
        total += sentence_len;
        if (i + 1 < collection->count && sentence_len > 0) {
            total++;  // space between sentences
        }
    }
    char *buffer = malloc(total + 1);
    if (!buffer) return -1;
    size_t pos = 0;
    for (size_t i = 0; i < collection->count; i++) {
        size_t start = pos;
        for (size_t w = 0; w < collection->sentences[i].word_count; w++) {
            if (w > 0) buffer[pos++] = ' ';
            if (collection->sentences[i].words[w].text) {
                size_t len = strlen(collection->sentences[i].words[w].text);
                memcpy(buffer + pos, collection->sentences[i].words[w].text, len);
                pos += len;
            }
        }
        size_t sentence_len = pos - start;
        offsets[i] = start;
        lengths[i] = sentence_len;
        if (i + 1 < collection->count && sentence_len > 0) {
            buffer[pos++] = ' ';
        }
    }
    buffer[pos] = '\0';
    *out_text = buffer;
    if (out_len) {
        *out_len = pos;
    }
    return 0;
}

static int rebuild_metadata_from_collection(FileMetadata *meta,
                                            const SentenceCollection *collection,
                                            const size_t *offsets,
                                            const size_t *lengths) {
    if (!meta || !collection) return -1;
    if ((int)collection->count > MAX_SENTENCE_METADATA) {
        return -1;
    }
    meta->sentence_count = (int)collection->count;
    int max_id = 0;
    for (size_t i = 0; i < collection->count; i++) {
        SentenceMeta *sm = &meta->sentences[i];
        sm->sentence_id = collection->sentences[i].sentence_id;
        sm->version = collection->sentences[i].version;
        sm->offset = offsets[i];
        sm->length = lengths[i];
        sm->word_count = (int)collection->sentences[i].word_count;
        sm->char_count = (int)lengths[i];
        if (sm->sentence_id > max_id) {
            max_id = sm->sentence_id;
        }
    }
    if (max_id >= meta->next_sentence_id) {
        meta->next_sentence_id = max_id + 1;
    }
    return 0;
}

static int find_sentence_index_by_id(const SentenceCollection *collection, int sentence_id) {
    if (!collection) return -1;
    for (size_t i = 0; i < collection->count; i++) {
        if (collection->sentences[i].sentence_id == sentence_id) {
            return (int)i;
        }
    }
    return -1;
}

static int find_metadata_index_by_id(const FileMetadata *meta, int sentence_id) {
    if (!meta) return -1;
    for (int i = 0; i < meta->sentence_count; i++) {
        if (meta->sentences[i].sentence_id == sentence_id) {
            return i;
        }
    }
    return -1;
}

static void format_error(char *buf, size_t len, const char *msg) {
    if (!buf || len == 0) return;
    snprintf(buf, len, "%s", msg ? msg : "unknown error");
}

int write_session_begin(WriteSession *session,
                        const char *storage_dir,
                        const char *filename,
                        int sentence_index,
                        const char *username,
                        char **current_text_out,
                        char *error_buf, size_t error_buf_len) {
    if (!session || !storage_dir || !filename || !username) {
        format_error(error_buf, error_buf_len, "Invalid parameters");
        return -1;
    }
    memset(session, 0, sizeof(*session));
    strncpy(session->storage_dir, storage_dir, sizeof(session->storage_dir) - 1);
    strncpy(session->filename, filename, sizeof(session->filename) - 1);
    strncpy(session->username, username, sizeof(session->username) - 1);
    session->sentence_index = sentence_index;

    FileMetadata meta;
    if (metadata_load(storage_dir, filename, &meta) != 0) {
        format_error(error_buf, error_buf_len, "Failed to load metadata");
        return -1;
    }
    if (metadata_ensure_sentences(storage_dir, filename, &meta) != 0) {
        format_error(error_buf, error_buf_len, "Failed to prepare sentence metadata");
        return -1;
    }
    if (sentence_index < 0 || sentence_index >= meta.sentence_count) {
        format_error(error_buf, error_buf_len, "Sentence index out of range");
        return -1;
    }
    int sentence_id = meta.sentences[sentence_index].sentence_id;
    if (sentence_id <= 0) {
        sentence_id = meta.next_sentence_id++;
        meta.sentences[sentence_index].sentence_id = sentence_id;
        metadata_save(storage_dir, filename, &meta);
    }
    int session_id;
    if (sentence_lock_acquire(filename, sentence_id, username, &session_id) < 0) {
        format_error(error_buf, error_buf_len, "Sentence is locked by another writer");
        return -1;
    }
    session->session_id = session_id;
    session->sentence_id = sentence_id;
    session->active = 1;

    char *file_text = NULL;
    size_t file_len = 0;
    if (file_read_all(storage_dir, filename, &file_text, &file_len) != 0) {
        format_error(error_buf, error_buf_len, "Failed to read file");
        write_session_abort(session);
        return -1;
    }
    SentenceCollection collection = {0};
    int next_sid = 1;
    if (sentence_parse_text(file_text, next_sid, &collection, &next_sid) != 0) {
        free(file_text);
        format_error(error_buf, error_buf_len, "Failed to parse file");
        write_session_abort(session);
        return -1;
    }
    assign_sentence_ids(&collection, &meta);
    if (sentence_index >= (int)collection.count) {
        sentence_collection_free(&collection);
        free(file_text);
        format_error(error_buf, error_buf_len, "Sentence index mismatch");
        write_session_abort(session);
        return -1;
    }
    if (sentence_entry_clone(&collection.sentences[sentence_index],
                             &session->sentence_entry) != 0) {
        sentence_collection_free(&collection);
        free(file_text);
        format_error(error_buf, error_buf_len, "Failed to prepare sentence copy");
        write_session_abort(session);
        return -1;
    }
    if (current_text_out) {
        *current_text_out = sentence_entry_to_string(&session->sentence_entry);
    }
    sentence_collection_free(&collection);
    free(file_text);
    return 0;
}

int write_session_apply_edit(WriteSession *session,
                             int word_index,
                             const char *content,
                             char *error_buf, size_t error_buf_len) {
    if (!session || !session->active) {
        format_error(error_buf, error_buf_len, "No active write session");
        return -1;
    }
    if (word_index < 0 || (size_t)word_index > session->sentence_entry.word_count) {
        format_error(error_buf, error_buf_len, "Word index out of range");
        return -1;
    }
    TokenList tokens = {0};
    if (split_content_into_tokens(content, &tokens) != 0) {
        format_error(error_buf, error_buf_len, "Content must contain at least one word");
        return -1;
    }
    if (sentence_entry_insert_tokens(&session->sentence_entry,
                                     (size_t)word_index, &tokens) != 0) {
        free_tokens(&tokens);
        format_error(error_buf, error_buf_len, "Failed to apply edit");
        return -1;
    }
    free_tokens(&tokens);
    return 0;
}

char *write_session_get_current_text(const WriteSession *session) {
    if (!session || !session->active) return NULL;
    return sentence_entry_to_string(&session->sentence_entry);
}

static int write_updated_file(WriteSession *session,
                              SentenceCollection *file_col,
                              FileMetadata *meta,
                              size_t *offsets,
                              size_t *lengths,
                              size_t total_words,
                              char **out_text,
                              size_t *out_len,
                              char *error_buf, size_t error_buf_len) {
    if (render_collection(file_col, out_text, out_len, offsets, lengths) != 0) {
        format_error(error_buf, error_buf_len, "Failed to render file");
        return -1;
    }
    char tmp_path[1024];
    char final_path[1024];
    snprintf(tmp_path, sizeof(tmp_path), "%s/files/%s.%d.tmp",
             session->storage_dir, session->filename, session->session_id);
    snprintf(final_path, sizeof(final_path), "%s/files/%s",
             session->storage_dir, session->filename);
    FILE *fp = fopen(tmp_path, "w");
    if (!fp) {
        format_error(error_buf, error_buf_len, "Failed to open temp file");
        return -1;
    }
    if (fwrite(*out_text, 1, *out_len, fp) != *out_len) {
        fclose(fp);
        unlink(tmp_path);
        format_error(error_buf, error_buf_len, "Failed to write temp file");
        return -1;
    }
    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);
    if (rename(tmp_path, final_path) != 0) {
        unlink(tmp_path);
        format_error(error_buf, error_buf_len, "Failed to commit file");
        return -1;
    }
    meta->word_count = (int)total_words;
    meta->char_count = (int)*out_len;
    meta->size_bytes = *out_len;
    meta->last_modified = time(NULL);
    meta->last_accessed = meta->last_modified;
    if (rebuild_metadata_from_collection(meta, file_col, offsets, lengths) != 0) {
        format_error(error_buf, error_buf_len, "Failed to refresh metadata");
        return -1;
    }
    if (metadata_save(session->storage_dir, session->filename, meta) != 0) {
        format_error(error_buf, error_buf_len, "Failed to save metadata");
        return -1;
    }
    return 0;
}

int write_session_commit(WriteSession *session,
                         char *error_buf, size_t error_buf_len) {
    if (!session || !session->active) {
        format_error(error_buf, error_buf_len, "No active write session");
        return -1;
    }
    if (undo_save_state(session->storage_dir, session->filename) != 0) {
        format_error(error_buf, error_buf_len, "Failed to snapshot undo state");
        return -1;
    }
    char *sentence_text = sentence_entry_to_string(&session->sentence_entry);
    if (!sentence_text) {
        format_error(error_buf, error_buf_len, "Failed to build sentence");
        return -1;
    }
    SentenceCollection fragment = {0};
    int next_id = session->sentence_id;
    if (sentence_parse_text(sentence_text, next_id, &fragment, &next_id) != 0) {
        free(sentence_text);
        format_error(error_buf, error_buf_len, "Failed to parse updated sentence");
        return -1;
    }
    if (fragment.count == 0) {
        sentence_collection_free(&fragment);
        free(sentence_text);
        format_error(error_buf, error_buf_len, "Sentence must contain words");
        return -1;
    }
    FileMetadata meta;
    if (metadata_load(session->storage_dir, session->filename, &meta) != 0) {
        sentence_collection_free(&fragment);
        free(sentence_text);
        format_error(error_buf, error_buf_len, "Failed to reload metadata");
        return -1;
    }
    if (metadata_ensure_sentences(session->storage_dir, session->filename, &meta) != 0) {
        sentence_collection_free(&fragment);
        free(sentence_text);
        format_error(error_buf, error_buf_len, "Failed to prepare metadata");
        return -1;
    }
    int meta_idx = find_metadata_index_by_id(&meta, session->sentence_id);
    if (meta_idx < 0) {
        sentence_collection_free(&fragment);
        free(sentence_text);
        format_error(error_buf, error_buf_len, "Sentence metadata missing");
        return -1;
    }
    fragment.sentences[0].sentence_id = session->sentence_id;
    fragment.sentences[0].version = meta.sentences[meta_idx].version + 1;
    for (size_t i = 1; i < fragment.count; i++) {
        fragment.sentences[i].sentence_id = meta.next_sentence_id++;
        fragment.sentences[i].version = 1;
    }

    char *file_text = NULL;
    size_t file_len = 0;
    if (file_read_all(session->storage_dir, session->filename, &file_text, &file_len) != 0) {
        sentence_collection_free(&fragment);
        free(sentence_text);
        format_error(error_buf, error_buf_len, "Failed to read file");
        return -1;
    }
    SentenceCollection file_col = {0};
    int parse_next = 1;
    if (sentence_parse_text(file_text, parse_next, &file_col, &parse_next) != 0) {
        sentence_collection_free(&fragment);
        free(sentence_text);
        free(file_text);
        format_error(error_buf, error_buf_len, "Failed to parse file");
        return -1;
    }
    assign_sentence_ids(&file_col, &meta);
    int file_idx = find_sentence_index_by_id(&file_col, session->sentence_id);
    if (file_idx < 0) {
        sentence_collection_free(&fragment);
        sentence_collection_free(&file_col);
        free(sentence_text);
        free(file_text);
        format_error(error_buf, error_buf_len, "Sentence not found in file");
        return -1;
    }
    SentenceEntry *old_entries = file_col.sentences;
    size_t old_count = file_col.count;
    size_t new_count = old_count - 1 + fragment.count;
    SentenceEntry *new_entries = calloc(new_count, sizeof(SentenceEntry));
    if (!new_entries) {
        sentence_collection_free(&fragment);
        sentence_collection_free(&file_col);
        free(sentence_text);
        free(file_text);
        format_error(error_buf, error_buf_len, "Out of memory");
        return -1;
    }
    size_t pos = 0;
    for (size_t i = 0; i < (size_t)file_idx; i++) {
        new_entries[pos++] = old_entries[i];
    }
    sentence_entry_clear(&old_entries[file_idx]);
    for (size_t i = 0; i < fragment.count; i++) {
        new_entries[pos++] = fragment.sentences[i];
        fragment.sentences[i].words = NULL;
        fragment.sentences[i].word_count = 0;
    }
    for (size_t i = file_idx + 1; i < old_count; i++) {
        new_entries[pos++] = old_entries[i];
    }
    free(old_entries);
    free(file_text);
    free(sentence_text);
    free(fragment.sentences);
    fragment.sentences = NULL;
    fragment.count = 0;
    file_col.sentences = new_entries;
    file_col.count = new_count;

    size_t *offsets = calloc(new_count, sizeof(size_t));
    size_t *lengths = calloc(new_count, sizeof(size_t));
    if (!offsets || !lengths) {
        free(offsets);
        free(lengths);
        sentence_collection_free(&file_col);
        format_error(error_buf, error_buf_len, "Out of memory");
        return -1;
    }
    char *rendered = NULL;
    size_t rendered_len = 0;
    size_t total_words = 0;
    for (size_t i = 0; i < file_col.count; i++) {
        total_words += file_col.sentences[i].word_count;
    }
    if (write_updated_file(session, &file_col, &meta, offsets, lengths,
                           total_words,
                           &rendered, &rendered_len,
                           error_buf, error_buf_len) != 0) {
        free(offsets);
        free(lengths);
        free(rendered);
        sentence_collection_free(&file_col);
        return -1;
    }
    free(rendered);
    free(offsets);
    free(lengths);
    sentence_collection_free(&file_col);
    write_session_abort(session);
    return 0;
}

void write_session_abort(WriteSession *session) {
    if (!session) return;
    if (session->active) {
        sentence_lock_release(session->filename, session->sentence_id, session->session_id);
    }
    sentence_entry_clear(&session->sentence_entry);
    memset(session, 0, sizeof(*session));
}


