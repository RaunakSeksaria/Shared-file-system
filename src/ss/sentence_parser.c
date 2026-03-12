#include "sentence_parser.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int is_sentence_delim(char c) {
    return (c == '.' || c == '!' || c == '?');
}

static void free_sentence(SentenceEntry *entry) {
    if (!entry || !entry->words) return;
    for (size_t i = 0; i < entry->word_count; i++) {
        free(entry->words[i].text);
    }
    free(entry->words);
    entry->words = NULL;
    entry->word_count = 0;
}

void sentence_collection_free(SentenceCollection *collection) {
    if (!collection || !collection->sentences) return;
    for (size_t i = 0; i < collection->count; i++) {
        free_sentence(&collection->sentences[i]);
    }
    free(collection->sentences);
    collection->sentences = NULL;
    collection->count = 0;
}

static int append_word(SentenceEntry *entry, const char *start, size_t len) {
    if (len == 0) return 0;
    SentenceWord *new_words = realloc(entry->words, sizeof(SentenceWord) * (entry->word_count + 1));
    if (!new_words) return -1;
    entry->words = new_words;
    char *buffer = (char *)malloc(len + 1);
    if (!buffer) return -1;
    memcpy(buffer, start, len);
    buffer[len] = '\0';
    entry->words[entry->word_count].text = buffer;
    entry->word_count += 1;
    return 0;
}

static int finalize_sentence(SentenceCollection *collection, SentenceEntry *current) {
    if (current->word_count == 0) {
        free_sentence(current);
        return 0;
    }
    SentenceEntry *new_array = realloc(collection->sentences, sizeof(SentenceEntry) * (collection->count + 1));
    if (!new_array) {
        free_sentence(current);
        return -1;
    }
    collection->sentences = new_array;
    collection->sentences[collection->count] = *current;
    collection->count++;
    memset(current, 0, sizeof(*current));
    return 0;
}

int sentence_parse_text(const char *text,
                        int start_sentence_id,
                        SentenceCollection *collection,
                        int *next_sentence_id_out) {
    if (!text || !collection || !next_sentence_id_out) return -1;
    memset(collection, 0, sizeof(*collection));

    int next_id = start_sentence_id;
    SentenceEntry current = {0};
    current.sentence_id = next_id++;
    current.version = 1;

    const char *p = text;
    const char *token_start = NULL;
    while (*p) {
        if (isspace((unsigned char)*p)) {
            if (token_start) {
                if (append_word(&current, token_start, (size_t)(p - token_start)) != 0) {
                    sentence_collection_free(collection);
                    free_sentence(&current);
                    return -1;
                }
                token_start = NULL;
            }
            p++;
            continue;
        }
        if (is_sentence_delim(*p)) {
            if (token_start) {
                if (append_word(&current, token_start, (size_t)(p - token_start + 1)) != 0) {
                    sentence_collection_free(collection);
                    free_sentence(&current);
                    return -1;
                }
                token_start = NULL;
            } else if (current.word_count > 0) {
                // Append delimiter to last word
                SentenceWord *last = &current.words[current.word_count - 1];
                size_t old_len = strlen(last->text);
                char *resized = realloc(last->text, old_len + 2);
                if (!resized) {
                    sentence_collection_free(collection);
                    free_sentence(&current);
                    return -1;
                }
                resized[old_len] = *p;
                resized[old_len + 1] = '\0';
                last->text = resized;
            }

            if (finalize_sentence(collection, &current) != 0) {
                sentence_collection_free(collection);
                return -1;
            }
            current.sentence_id = next_id++;
            current.version = 1;
            p++;
            continue;
        }

        if (!token_start) {
            token_start = p;
        }
        p++;
    }

    if (token_start) {
        if (append_word(&current, token_start, (size_t)(p - token_start)) != 0) {
            sentence_collection_free(collection);
            free_sentence(&current);
            return -1;
        }
    }

        if (finalize_sentence(collection, &current) != 0) {
        sentence_collection_free(collection);
        return -1;
    }
    if (collection->count == 0) {
        SentenceEntry *arr = realloc(collection->sentences, sizeof(SentenceEntry));
        if (!arr) {
            sentence_collection_free(collection);
            return -1;
        }
        collection->sentences = arr;
        memset(&collection->sentences[0], 0, sizeof(SentenceEntry));
        collection->sentences[0].sentence_id = next_id++;
        collection->sentences[0].version = 1;
        collection->count = 1;
    }

    *next_sentence_id_out = next_id;
    return 0;
}

int sentence_render_text(const SentenceCollection *collection, char **out_text, size_t *out_len) {
    if (!collection || !out_text || !out_len) return -1;
    size_t capacity = 1024;
    char *buffer = (char *)malloc(capacity);
    if (!buffer) return -1;
    size_t written = 0;

    for (size_t i = 0; i < collection->count; i++) {
        const SentenceEntry *entry = &collection->sentences[i];
        for (size_t w = 0; w < entry->word_count; w++) {
            const char *word = entry->words[w].text ? entry->words[w].text : "";
            size_t len = strlen(word);
            // Add space if needed
            if (written > 0 && written + len + 1 >= capacity) {
                capacity *= 2;
                char *tmp = realloc(buffer, capacity);
                if (!tmp) {
                    free(buffer);
                    return -1;
                }
                buffer = tmp;
            } else if (written + len + 1 >= capacity) {
                capacity *= 2;
                char *tmp = realloc(buffer, capacity);
                if (!tmp) {
                    free(buffer);
                    return -1;
                }
                buffer = tmp;
            }

            if (written > 0) {
                buffer[written++] = ' ';
            }
            memcpy(buffer + written, word, len);
            written += len;
        }
        if (i + 1 < collection->count) {
            if (written + 1 >= capacity) {
                capacity *= 2;
                char *tmp = realloc(buffer, capacity);
                if (!tmp) {
                    free(buffer);
                    return -1;
                }
                buffer = tmp;
            }
            buffer[written++] = ' ';
        }
    }

    buffer[written] = '\0';
    *out_text = buffer;
    *out_len = written;
    return 0;
}


