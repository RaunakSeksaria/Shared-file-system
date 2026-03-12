#ifndef SENTENCE_PARSER_H
#define SENTENCE_PARSER_H

#include <stddef.h>

#define SENTENCE_PARSER_MAX_WORD_LEN 512

typedef struct SentenceWord {
    char *text;
} SentenceWord;

typedef struct SentenceEntry {
    int sentence_id;
    int version;
    SentenceWord *words;
    size_t word_count;
} SentenceEntry;

typedef struct SentenceCollection {
    SentenceEntry *sentences;
    size_t count;
} SentenceCollection;

// Parse raw text into a collection of sentences and words.
// start_sentence_id: first ID to assign; next_sentence_id_out receives next available ID.
// Returns 0 on success.
int sentence_parse_text(const char *text,
                        int start_sentence_id,
                        SentenceCollection *collection,
                        int *next_sentence_id_out);

// Render a SentenceCollection back into a single text buffer.
// Caller owns *out_text (allocated with malloc). out_len receives length (excluding null terminator).
int sentence_render_text(const SentenceCollection *collection, char **out_text, size_t *out_len);

// Free all memory held by the collection.
void sentence_collection_free(SentenceCollection *collection);

#endif  // SENTENCE_PARSER_H


