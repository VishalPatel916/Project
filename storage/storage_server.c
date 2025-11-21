#include "protocol.h"
#include <netinet/in.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/select.h>
#include <fcntl.h>
#include <malloc.h> 
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>

#define MY_IP_FOR_CLIENTS "127.0.0.1"
#define MY_PORT_FOR_CLIENTS 8082
#define MY_STORAGE_PATH "./ss_storage"

#define MAX_CONNECTIONS FD_SETSIZE
#define MAX_LOCKS 100

// --- Global Configuration (overridable via command-line) ---
char g_storage_path[512] = MY_STORAGE_PATH;

// --- Global Log File ---
FILE* ss_log_file;

// --- 1. NEW LINKED LIST DATA STRUCTURES ---
typedef struct WordNode {
    char* word;
    struct WordNode* next;
} WordNode;

typedef struct SentenceNode {
    WordNode* word_head;
    char delimiter; // The sentence terminator ('.', '!', '?', or '\n')
    struct SentenceNode* next;
} SentenceNode;

// --- 2. UPDATED GLOBAL STATE STRUCTS ---
typedef struct { int active; char filename[MAX_FILENAME]; SentenceNode* sentence_ptr; int sock_fd; } LockInfo;
LockInfo global_locks[MAX_LOCKS]; 

typedef struct {
    int active;
    char filename[MAX_FILENAME];
    SentenceNode* doc_head; 
    int num_users_editing;  
    char original_path[768];
    char backup_path[768];
} ActiveDoc;
ActiveDoc active_documents[MAX_FILES_IN_SYSTEM];

// Track individual edit operations for merging
typedef struct {
    int word_index;
    char content[256];
} EditOperation;

typedef struct {
    int active;
    int doc_index;
    SentenceNode* sentence_ptr;  // Pointer to locked sentence (stable across splits)
    int original_word_count;  // Word count when session started
    EditOperation* edit_ops;  // Array of edit operations
    int edit_count;           // Number of operations recorded
    int edit_capacity;        // Capacity of edit_ops array
    int virtual_word_count;   // Sum of words sent in write updates before ETIRW
} WriteSession;
WriteSession write_sessions[MAX_CONNECTIONS]; 


// --- 3. Logging Function (Same as before) ---
void log_event(const char* format, ...) {
    char time_buf[50]; time_t now = time(NULL); strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", localtime(&now));
    va_list args;
    printf("[%s] ", time_buf); va_start(args, format); vprintf(format, args); va_end(args); printf("\n");
    fprintf(ss_log_file, "[%s] ", time_buf); va_start(args, format); vfprintf(ss_log_file, format, args); va_end(args); fprintf(ss_log_file, "\n");
    fflush(ss_log_file);
}

// --- 4. Lock Helpers (Updated to use pointers) ---
void init_locks() { for (int i = 0; i < MAX_LOCKS; i++) global_locks[i].active = 0; }
int find_lock(char* f, SentenceNode* s) { for (int i = 0; i < MAX_LOCKS; i++) if (global_locks[i].active && !strcmp(global_locks[i].filename, f) && global_locks[i].sentence_ptr == s) return i; return -1; }
int create_lock(char* f, SentenceNode* s, int sock) { if (find_lock(f, s) != -1) return 0; for (int i = 0; i < MAX_LOCKS; i++) if (!global_locks[i].active) { global_locks[i].active = 1; strncpy(global_locks[i].filename, f, MAX_FILENAME); global_locks[i].sentence_ptr = s; global_locks[i].sock_fd = sock; log_event("  -> Lock CREATED for '%s' (sentence ptr %p) by socket %d", f, (void*)s, sock); return 1; } return -1; }
void release_lock(char* f, SentenceNode* s) { int i = find_lock(f, s); if (i != -1) { global_locks[i].active = 0; log_event("  -> Lock RELEASED for '%s' (sentence ptr %p)", f, (void*)s); } }

// --- 5. Metadata Persistence ---
#define METADATA_FILE "./ss_storage/.metadata"

typedef struct {
    char filename[MAX_FILENAME];
    char owner[MAX_USERNAME];
    int access_count;
    AccessEntry access_list[MAX_PERMISSIONS_PER_FILE];
} FileMetadata_SS;

void save_file_metadata(char* filename, char* owner, int access_count, AccessEntry* access_list) {
    FILE* f = fopen(METADATA_FILE, "r");
    FileMetadata_SS metadata[MAX_FILES_PER_SS];
    int meta_count = 0;
    
    if (f != NULL) {
        if (fscanf(f, "%d\n", &meta_count) == 1) {
            for (int i = 0; i < meta_count && i < MAX_FILES_PER_SS; i++) {
                fgets(metadata[i].filename, MAX_FILENAME, f);
                metadata[i].filename[strcspn(metadata[i].filename, "\n")] = 0;
                fgets(metadata[i].owner, MAX_USERNAME, f);
                metadata[i].owner[strcspn(metadata[i].owner, "\n")] = 0;
                fscanf(f, "%d\n", &metadata[i].access_count);
                for (int j = 0; j < metadata[i].access_count; j++) {
                    fscanf(f, "%s %d\n", metadata[i].access_list[j].username, (int*)&metadata[i].access_list[j].permission);
                }
            }
        }
        fclose(f);
    }
    
    int found = 0;
    for (int i = 0; i < meta_count; i++) {
        if (strcmp(metadata[i].filename, filename) == 0) {
            strncpy(metadata[i].owner, owner, MAX_USERNAME);
            metadata[i].access_count = access_count;
            for (int j = 0; j < access_count; j++) {
                metadata[i].access_list[j] = access_list[j];
            }
            found = 1;
            break;
        }
    }
    
    if (!found && meta_count < MAX_FILES_PER_SS) {
        strncpy(metadata[meta_count].filename, filename, MAX_USERNAME);
        strncpy(metadata[meta_count].owner, owner, MAX_USERNAME);
        metadata[meta_count].access_count = access_count;
        for (int j = 0; j < access_count; j++) {
            metadata[meta_count].access_list[j] = access_list[j];
        }
        meta_count++;
    }
    
    f = fopen(METADATA_FILE, "w");
    if (f == NULL) { log_event("ERROR: Failed to save metadata"); return; }
    fprintf(f, "%d\n", meta_count);
    for (int i = 0; i < meta_count; i++) {
        fprintf(f, "%s\n%s\n%d\n", metadata[i].filename, metadata[i].owner, metadata[i].access_count);
        for (int j = 0; j < metadata[i].access_count; j++) {
            fprintf(f, "%s %d\n", metadata[i].access_list[j].username, metadata[i].access_list[j].permission);
        }
    }
    fclose(f);
    log_event("  -> Metadata saved for '%s'", filename);
}

void load_file_metadata(char* filename, char* owner_out, int* access_count_out, AccessEntry* access_list_out) {
    FILE* f = fopen(METADATA_FILE, "r");
    if (f == NULL) {
        strcpy(owner_out, "system");
        *access_count_out = 0;
        return;
    }
    
    int meta_count;
    if (fscanf(f, "%d\n", &meta_count) != 1) {
        fclose(f);
        strcpy(owner_out, "system");
        *access_count_out = 0;
        return;
    }
    
    for (int i = 0; i < meta_count; i++) {
        char fname[MAX_FILENAME], owner[MAX_USERNAME];
        int access_count;
        
        fgets(fname, MAX_FILENAME, f);
        fname[strcspn(fname, "\n")] = 0;
        fgets(owner, MAX_USERNAME, f);
        owner[strcspn(owner, "\n")] = 0;
        fscanf(f, "%d\n", &access_count);
        
        if (strcmp(fname, filename) == 0) {
            strncpy(owner_out, owner, MAX_USERNAME);
            *access_count_out = access_count;
            for (int j = 0; j < access_count; j++) {
                fscanf(f, "%s %d\n", access_list_out[j].username, (int*)&access_list_out[j].permission);
            }
            fclose(f);
            return;
        } else {
            for (int j = 0; j < access_count; j++) {
                char dummy_user[MAX_USERNAME];
                int dummy_perm;
                fscanf(f, "%s %d\n", dummy_user, &dummy_perm);
            }
        }
    }
    
    fclose(f);
    strcpy(owner_out, "system");
    *access_count_out = 0;
}

void delete_file_metadata(char* filename) {
    FILE* f = fopen(METADATA_FILE, "r");
    if (f == NULL) return;
    
    FileMetadata_SS metadata[MAX_FILES_PER_SS];
    int meta_count = 0;
    
    if (fscanf(f, "%d\n", &meta_count) == 1) {
        for (int i = 0; i < meta_count && i < MAX_FILES_PER_SS; i++) {
            fgets(metadata[i].filename, MAX_FILENAME, f);
            metadata[i].filename[strcspn(metadata[i].filename, "\n")] = 0;
            fgets(metadata[i].owner, MAX_USERNAME, f);
            metadata[i].owner[strcspn(metadata[i].owner, "\n")] = 0;
            fscanf(f, "%d\n", &metadata[i].access_count);
            for (int j = 0; j < metadata[i].access_count; j++) {
                fscanf(f, "%s %d\n", metadata[i].access_list[j].username, (int*)&metadata[i].access_list[j].permission);
            }
        }
    }
    fclose(f);
    
    f = fopen(METADATA_FILE, "w");
    if (f == NULL) return;
    
    int new_count = 0;
    for (int i = 0; i < meta_count; i++) {
        if (strcmp(metadata[i].filename, filename) != 0) {
            new_count++;
        }
    }
    
    fprintf(f, "%d\n", new_count);
    for (int i = 0; i < meta_count; i++) {
        if (strcmp(metadata[i].filename, filename) != 0) {
            fprintf(f, "%s\n%s\n%d\n", metadata[i].filename, metadata[i].owner, metadata[i].access_count);
            for (int j = 0; j < metadata[i].access_count; j++) {
                fprintf(f, "%s %d\n", metadata[i].access_list[j].username, metadata[i].access_list[j].permission);
            }
        }
    }
    fclose(f);
}

// --- 6. Sentence/Word Linked List Helpers ---

// --- 5. NEW Linked List Helper Functions ---
WordNode* create_word_node(const char* word_str) {
    WordNode* node = (WordNode*)malloc(sizeof(WordNode));
    node->word = strdup(word_str); 
    node->next = NULL;
    return node;
}
SentenceNode* create_sentence_node(char delim) {
    SentenceNode* node = (SentenceNode*)malloc(sizeof(SentenceNode));
    node->word_head = NULL;
    node->delimiter = delim;
    node->next = NULL;
    return node;
}
void free_document(SentenceNode* sent_head) {
    SentenceNode* current_sent = sent_head;
    while (current_sent != NULL) {
        WordNode* current_word = current_sent->word_head;
        while (current_word != NULL) {
            WordNode* next_word = current_word->next;
            free(current_word->word); free(current_word);
            current_word = next_word;
        }
        SentenceNode* next_sent = current_sent->next;
        free(current_sent); current_sent = next_sent;
    }
}

// Free only a single sentence node and its words (do not touch next)
void free_sentence_node(SentenceNode* sent) {
    if (!sent) return;
    WordNode* current_word = sent->word_head;
    while (current_word != NULL) {
        WordNode* next_word = current_word->next;
        free(current_word->word); free(current_word);
        current_word = next_word;
    }
    free(sent);
}

// *** THIS FUNCTION IS REWRITTEN TO BE SAFER ***
SentenceNode* parse_file_to_list(const char* file_path) {
    FILE* f = fopen(file_path, "r");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END); long file_size = ftell(f); rewind(f);
    char* buffer = (char*)malloc(file_size + 1);
    fread(buffer, 1, file_size, f);
    buffer[file_size] = '\0';
    fclose(f);

    SentenceNode* doc_head = NULL;
    SentenceNode* current_sent = NULL;
    WordNode* current_word = NULL;

    char* word_buffer = (char*)malloc(file_size + 1); // Buffer for the current word
    int word_idx = 0;
    
    // Start with a new sentence
    doc_head = create_sentence_node(' ');
    current_sent = doc_head;

    for (int i = 0; i < file_size; i++) {
        char c = buffer[i];

        if (c == '.' || c == '!' || c == '?') {
            // Found a sentence delimiter
            if (word_idx > 0) { // Save the last word
                word_buffer[word_idx] = '\0';
                WordNode* new_word = create_word_node(word_buffer);
                if (current_word == NULL) current_sent->word_head = new_word;
                else current_word->next = new_word;
                current_word = new_word;
                word_idx = 0;
            }
            
            // Create a new sentence
            current_sent->delimiter = c;
            SentenceNode* new_sent = create_sentence_node(' ');
            current_sent->next = new_sent;
            current_sent = new_sent;
            current_word = NULL;
        } 
        else if (isspace(c)) { // Found a word delimiter
            if (word_idx > 0) { // Save the last word
                word_buffer[word_idx] = '\0';
                WordNode* new_word = create_word_node(word_buffer);
                if (current_word == NULL) current_sent->word_head = new_word;
                else current_word->next = new_word;
                current_word = new_word;
                word_idx = 0;
            }
            // If it's a newline, it's also a sentence
            if (c == '\n') {
                current_sent->delimiter = '\n';
                SentenceNode* new_sent = create_sentence_node(' ');
                current_sent->next = new_sent;
                current_sent = new_sent;
                current_word = NULL;
            }
        } 
        else {
            // Just a normal character, add to word
            word_buffer[word_idx++] = c;
        }
    }

    // Save any trailing word
    if (word_idx > 0) {
        word_buffer[word_idx] = '\0';
        WordNode* new_word = create_word_node(word_buffer);
        if (current_word == NULL) current_sent->word_head = new_word;
        else current_word->next = new_word;
    }

    free(buffer);
    free(word_buffer);
    return doc_head;
}

// This function writes the linked list structure back to the physical file
void flush_list_to_file(SentenceNode* sent_head, const char* file_path) {
    FILE* f = fopen(file_path, "w");
    if (!f) { log_event("  -> ERROR: Could not open file for flushing: %s", file_path); return; }

    SentenceNode* current_sent = sent_head;
    while (current_sent != NULL) {
        WordNode* current_word = current_sent->word_head;
        while (current_word != NULL) {
            fprintf(f, "%s", current_word->word);
            if (current_word->next != NULL) {
                fprintf(f, " "); // Add space between words
            }
            current_word = current_word->next;
        }
        
        if(current_sent->delimiter == '\n') {
            fprintf(f, "\n");
        } else if (current_sent->delimiter != ' ' || current_sent->next != NULL) {
            // Add delimiter if it's not a space, OR if it's a space
            // but not the very last sentence.
            fprintf(f, "%c ", current_sent->delimiter);
        }
        
        current_sent = current_sent->next;
    }
    fclose(f);
}

// This is the new, complex function that handles word insertion AND sentence splitting
// It now takes the doc_head directly
void handle_write_update_list(SentenceNode* doc_head, int sent_num, int word_idx, char* content) {
    // 1. Find the target sentence
    SentenceNode* target_sent = doc_head;
    for (int i = 0; i < sent_num && target_sent != NULL; i++) {
        target_sent = target_sent->next;
    }
    if (target_sent == NULL) {
        log_event("  -> ERROR: Sentence number %d out of bounds.", sent_num);
        return;
    }

    // 2. Find the insertion point (the node *before* word_idx)
    WordNode* insertion_point_prev = NULL;
    WordNode* insertion_point_next = target_sent->word_head;
    for (int i = 0; i < word_idx && insertion_point_next != NULL; i++) {
        insertion_point_prev = insertion_point_next;
        insertion_point_next = insertion_point_next->next;
    }

    // 3. Tokenize the new content by spaces
    char* content_copy = strdup(content); // Use a copy so strtok doesn't destroy original
    char* content_context = NULL;
    char* content_word = strtok_r(content_copy, " \t\r", &content_context);
    
    while(content_word != NULL) {
        // 4. Check *this word* for a delimiter
        char* delim_ptr = strpbrk(content_word, ".!?");
        
        if (delim_ptr != NULL) {
            // --- This word contains a delimiter! ---
            char delim_char = *delim_ptr;
            *delim_ptr = '\0'; 
            
            if (strlen(content_word) > 0) {
                WordNode* new_word = create_word_node(content_word);
                if (insertion_point_prev == NULL) target_sent->word_head = new_word;
                else insertion_point_prev->next = new_word;
                new_word->next = NULL; 
                insertion_point_prev = new_word;
            }
            
            SentenceNode* new_sent = create_sentence_node(target_sent->delimiter); 
            target_sent->delimiter = delim_char;
            
            new_sent->next = target_sent->next;
            target_sent->next = new_sent;
            new_sent->word_head = insertion_point_next;
            
            content_word = delim_ptr + 1;
            if (strlen(content_word) > 0) {
                WordNode* final_word = create_word_node(content_word);
                final_word->next = new_sent->word_head; 
                new_sent->word_head = final_word;
                insertion_point_prev = final_word;
            } else {
                insertion_point_prev = NULL;
            }

            target_sent = new_sent;
            insertion_point_next = target_sent->word_head;

        } else {
            // --- Simple word, no delimiter ---
            WordNode* new_word = create_word_node(content_word);
            if (insertion_point_prev == NULL) target_sent->word_head = new_word;
            else insertion_point_prev->next = new_word;
            new_word->next = insertion_point_next;
            insertion_point_prev = new_word;
        }
        
        content_word = strtok_r(NULL, " \t\r", &content_context);
    }
    free(content_copy);
}

// Count words in a content string (tokens separated by whitespace)
// Don't count standalone delimiters as words
static int count_words_in_string(const char* s) {
    if (!s) return 0;
    char* copy = strdup(s);
    char* ctx = NULL;
    char* tok = strtok_r(copy, " \t\r\n", &ctx);
    int cnt = 0;
    while (tok) {
        // Only count if it's not a standalone delimiter
        if (!(strlen(tok) == 1 && strchr(".!?\n", tok[0]))) {
            cnt++;
        }
        tok = strtok_r(NULL, " \t\r\n", &ctx);
    }
    free(copy);
    return cnt;
}

// Apply all edit operations from a session atomically to the target sentence.
// This builds a word-array for the sentence, applies all inserts, then
// splits into sentences only after all edits have been applied.
// Key: Each edit's word_index is relative to the sentence state when client sent it,
// so we must adjust for previous insertions.
static int apply_session_edits_to_sentence(ActiveDoc* doc, SentenceNode* target, WriteSession* session) {
    if (!doc || !target || !session) return -1;

    // Save the original sentence delimiter - we'll need it if no new delimiters are added
    char original_delimiter = target->delimiter;

    // 1. Build initial word list for the target sentence
    int orig_count = 0;
    WordNode* w = target->word_head;
    while (w) { orig_count++; w = w->next; }

    // Build dynamic array of strings
    int arr_capacity = orig_count + 256 + session->edit_count * 10;
    char** words = (char**)malloc(sizeof(char*) * (arr_capacity + 1));
    int idx = 0;
    w = target->word_head;
    while (w) { words[idx++] = strdup(w->word); w = w->next; }

    // 2. Apply each edit op in sequence, tracking cumulative insertions
    // Each edit's word_index refers to the position in the current array state
    for (int i = 0; i < session->edit_count; i++) {
        int insert_at = session->edit_ops[i].word_index;
        
        // Validate insertion point
        if (insert_at < 0 || insert_at > idx) {
            log_event("  -> ERROR: Edit %d has invalid word_index %d (array size %d)", i, insert_at, idx);
            for (int j = 0; j < idx; j++) free(words[j]);
            free(words);
            return -1;
        }
        
        // Tokenize content into words (split on whitespace, preserve delimiters IN words)
        char* copy = strdup(session->edit_ops[i].content);
        char* ctx = NULL;
        char* tok = strtok_r(copy, " \t\r\n", &ctx);
        
        while (tok) {
            // Ensure capacity
            if (idx + 1 > arr_capacity) {
                arr_capacity *= 2;
                words = (char**)realloc(words, sizeof(char*) * (arr_capacity + 1));
            }
            
            // Shift tail right to make room at insert_at
            for (int s = idx; s > insert_at; s--) {
                words[s] = words[s-1];
            }
            
            // Insert new word token
            words[insert_at] = strdup(tok);
            insert_at++;  // next token from same edit goes right after
            idx++;        // array grew
            
            tok = strtok_r(NULL, " \t\r\n", &ctx);
        }
        free(copy);
    }

    // 3. Now build new sentence nodes by splitting at delimiters
    // Delimiters can appear inside word strings (e.g., "hello." or "world!")
    // Key: We accumulate words into current sentence until we hit a delimiter
    // Each sentence starts with original_delimiter, overridden if a delimiter is found in content
    // Multiple consecutive delimiters (e.g., "cd...") create multiple empty sentences
    SentenceNode* first_new = NULL;
    SentenceNode* last_new = NULL;
    int cur = 0;
    
    while (cur < idx) {
        // Use original_delimiter as default for all sentences
        SentenceNode* snode = create_sentence_node(original_delimiter);
        snode->word_head = NULL;
        WordNode* last_word = NULL;
        
        while (cur < idx) {
            char* word_str = words[cur];
            
            // Check if this word contains a delimiter
            char* delim_pos = strpbrk(word_str, ".!?\n");
            
            if (delim_pos) {
                // Word contains delimiter - this ends the current sentence
                char delim_char = *delim_pos;
                
                // Part before delimiter becomes a word (if non-empty)
                int before_len = delim_pos - word_str;
                if (before_len > 0) {
                    char before_delim[MAX_WORD_CONTENT];
                    strncpy(before_delim, word_str, before_len);
                    before_delim[before_len] = '\0';
                    
                    WordNode* wn = create_word_node(before_delim);
                    if (!snode->word_head) snode->word_head = wn;
                    else last_word->next = wn;
                    last_word = wn;
                }
                
                // Override with the delimiter found in this word
                snode->delimiter = delim_char;
                
                // Check if there are MORE delimiters after this one (e.g., "cd..." has 3 dots)
                // We need to create empty sentences for each additional delimiter
                char* remaining = delim_pos + 1; // Start after first delimiter
                while (*remaining != '\0') {
                    if (strchr(".!?\n", *remaining)) {
                        // Another delimiter found - finish current sentence and create empty one
                        if (!first_new) first_new = snode;
                        else last_new->next = snode;
                        last_new = snode;
                        
                        // Create empty sentence with this delimiter
                        snode = create_sentence_node(*remaining);
                        snode->word_head = NULL;
                        last_word = NULL;
                        remaining++;
                    } else {
                        // Not a delimiter - this shouldn't happen with "cd..." but handle it
                        break;
                    }
                }
                
                cur++;
                break; // Move to next word (next sentence will be created in outer loop)
                
            } else {
                // Regular word, no delimiter - add to current sentence
                WordNode* wn = create_word_node(word_str);
                if (!snode->word_head) snode->word_head = wn;
                else last_word->next = wn;
                last_word = wn;
                cur++;
            }
        }
        
        // Attach sentence to list
        if (!first_new) first_new = snode;
        else last_new->next = snode;
        last_new = snode;
    }

    // 4. Replace target sentence in doc with new sentences
    SentenceNode* prev = NULL;
    SentenceNode* t = doc->doc_head;
    while (t && t != target) { prev = t; t = t->next; }
    
    if (!t) {
        log_event("  -> ERROR: Target sentence not found in document");
        for (int i = 0; i < idx; i++) free(words[i]);
        free(words);
        return -1;
    }

    // Link: prev -> first_new -> ... -> last_new -> target->next
    if (prev) prev->next = first_new;
    else doc->doc_head = first_new;
    
    if (last_new) last_new->next = target->next;

    // Free only the original target sentence (not its next chain)
    free_sentence_node(target);

    // Free word array
    for (int i = 0; i < idx; i++) free(words[i]);
    free(words);
    
    return 0;
}


// --- 6. NEW Active Document Helpers ---
int find_empty_active_doc_slot() {
    for (int i = 0; i < MAX_FILES_IN_SYSTEM; i++) { if (!active_documents[i].active) return i; }
    return -1;
}
int find_active_doc(char* filename) {
    for (int i = 0; i < MAX_FILES_IN_SYSTEM; i++) {
        if (active_documents[i].active && strcmp(active_documents[i].filename, filename) == 0) {
            return i;
        }
    }
    return -1;
}

ActiveDoc* find_or_load_active_doc(char* filename) {
    int doc_idx = find_active_doc(filename);
    if (doc_idx != -1) {
        log_event("  -> File '%s' is already active in memory.", filename);
        return &active_documents[doc_idx];
    }
    int new_slot = find_empty_active_doc_slot();
    if (new_slot == -1) { log_event("  -> ERROR: Active document list is full!"); return NULL; }
    
    ActiveDoc* doc = &active_documents[new_slot];
    log_event("  -> Loading file '%s' into active doc slot %d.", filename, new_slot);
    
    sprintf(doc->original_path, "%s/%s", g_storage_path, filename);
    sprintf(doc->backup_path, "%s/%s.bak", g_storage_path, filename);
    
    doc->doc_head = parse_file_to_list(doc->original_path);
    if (doc->doc_head == NULL) { log_event("  -> ERROR: Failed to parse file '%s' into list.", filename); return NULL; }

    doc->active = 1;
    strncpy(doc->filename, filename, MAX_FILENAME);
    doc->num_users_editing = 0;
    
    return doc;
}

void release_active_doc(ActiveDoc* doc) {
    doc->num_users_editing--;
    log_event("  -> User stopped editing '%s'. Active users: %d", doc->filename, doc->num_users_editing);
    if (doc->num_users_editing <= 0) {
        log_event("  -> No users left. Freeing document '%s' from memory.", doc->filename);
        free_document(doc->doc_head);
        doc->active = 0;
    }
}

// --- 7. Metadata, File Ops, & Disconnect Helpers ---
void calculate_and_send_metadata(int nm_sock, char* filename, char* file_path) {
    FILE* f = fopen(file_path, "r");
    if (f == NULL) { log_event("  -> ERROR: Could not open file %s to calculate metadata.", file_path); return; }
    long file_size = 0; int word_count = 0; int char_count = 0; int in_word = 0; char c;
    fseek(f, 0, SEEK_END); file_size = ftell(f); rewind(f);
    while ((c = fgetc(f)) != EOF) {
        char_count++;
        if (isspace(c)) { in_word = 0; } else { if (in_word == 0) { word_count++; in_word = 1; } }
    }
    fclose(f);
    struct stat st; time_t mod_time = 0; if (stat(file_path, &st) == 0) { mod_time = st.st_mtime; }
    log_event("  -> Calculated stats for '%s': size=%ld, words=%d, chars=%d", filename, file_size, word_count, char_count);
    Header header; header.type = REQ_UPDATE_METADATA; header.payload_size = sizeof(Msg_Update_Metadata);
    Msg_Update_Metadata msg;
    strncpy(msg.filename, filename, MAX_FILENAME);
    msg.file_size = file_size; msg.word_count = word_count; msg.char_count = char_count;
    msg.last_modified = mod_time;
    if (send(nm_sock, &header, sizeof(header), 0) < 0) log_event("  -> ERROR: send metadata header failed");
    if (send(nm_sock, &msg, sizeof(msg), 0) < 0) log_event("  -> ERROR: send metadata payload failed");
}
void handle_create_file(char* filename, char* file_path) {
    sprintf(file_path, "%s/%s", g_storage_path, filename);
    log_event("  -> Creating file at: %s", file_path);
    FILE* f = fopen(file_path, "w");
    if (f) { fclose(f); }
}
void handle_send_file(int client_sock, char* filename) { char file_path[768]; sprintf(file_path, "%s/%s", g_storage_path, filename); int fd = open(file_path, O_RDONLY); if (fd < 0) { log_event("  -> File not found. Sending error to socket %d", client_sock); send_simple_header(client_sock, RES_ERROR_NOT_FOUND); return; } send_simple_header(client_sock, RES_SS_FILE_OK); log_event("  -> Sending file '%s' to socket %d", filename, client_sock); char buffer[FILE_BUFFER_SIZE]; int bytes_read; while ((bytes_read = read(fd, buffer, FILE_BUFFER_SIZE)) > 0) if (send(client_sock, buffer, bytes_read, 0) < 0) break; close(fd); log_event("  -> Finished sending file to socket %d", client_sock); }
void handle_stream_file(int client_sock, char* filename) {
    char file_path[768]; sprintf(file_path, "%s/%s", g_storage_path, filename);
    int fd = open(file_path, O_RDONLY);
    if (fd < 0) { log_event("  -> File not found. Sending error to socket %d", client_sock); send_simple_header(client_sock, RES_ERROR_NOT_FOUND); return; }
    send_simple_header(client_sock, RES_SS_FILE_OK);
    lseek(fd, 0, SEEK_END); long file_size = lseek(fd, 0, SEEK_CUR); lseek(fd, 0, SEEK_SET);
    char* mem_buffer = (char*)malloc(file_size + 1);
    if (!mem_buffer) { perror("malloc stream buffer"); close(fd); return; }
    read(fd, mem_buffer, file_size); mem_buffer[file_size] = '\0'; close(fd);
    log_event("  -> Streaming file '%s' to socket %d", filename, client_sock);
    char* word_context = NULL; char* word = strtok_r(mem_buffer, " \n\t", &word_context);
    while (word != NULL) {
        if (send(client_sock, word, strlen(word), 0) < 0) break;
        if (send(client_sock, " ", 1, 0) < 0) break;
        usleep(100000); 
        word = strtok_r(NULL, " \n\t", &word_context);
    }
    free(mem_buffer); log_event("  -> Finished streaming file to socket %d", client_sock);
}

void handle_client_disconnect(int sock_fd, fd_set* master_set) {
    struct sockaddr_in addr; socklen_t len = sizeof(addr);
    char ip_buf[MAX_IP_LEN] = "UNKNOWN_IP";
    if (getpeername(sock_fd, (struct sockaddr*)&addr, &len) == 0) { strncpy(ip_buf, inet_ntoa(addr.sin_addr), MAX_IP_LEN); }
    log_event("Client on socket %d (%s) disconnected", sock_fd, ip_buf);
    
    if (write_sessions[sock_fd].active) {
        WriteSession* session = &write_sessions[sock_fd];
        ActiveDoc* doc = &active_documents[session->doc_index];
        log_event("  -> Client was in a write session!");
        
        release_lock(doc->filename, session->sentence_ptr);
        release_active_doc(doc);
        
        // Clean up edit operations
        if (session->edit_ops != NULL) {
            free(session->edit_ops);
            session->edit_ops = NULL;
        }
        session->active = 0;
    }
    close(sock_fd);
    FD_CLR(sock_fd, master_set);
}

// --- Recursive directory scanner ---
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
void scan_directory_recursive(const char* base_path, const char* relative_path, 
                               char files[][MAX_FILENAME], int* count, int max_count) {
    char full_path[1024];
    if (relative_path && strlen(relative_path) > 0) {
        snprintf(full_path, sizeof(full_path), "%s/%s", base_path, relative_path);
    } else {
        strncpy(full_path, base_path, sizeof(full_path));
        full_path[sizeof(full_path) - 1] = '\0';
    }
    
    DIR *d = opendir(full_path);
    if (!d) return;
    
    struct dirent *dir;
    while ((dir = readdir(d)) != NULL && *count < max_count) {
        if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0) continue;
        if (strcmp(dir->d_name, ".metadata") == 0) continue;
        
        // Check path length before concatenating
        if (strlen(full_path) + strlen(dir->d_name) + 2 > 1024) continue;
        
        char item_path[1024];
        snprintf(item_path, sizeof(item_path), "%s/%s", full_path, dir->d_name);
        
        struct stat statbuf;
        if (stat(item_path, &statbuf) == 0) {
            if (S_ISDIR(statbuf.st_mode)) {
                // Directory - recurse into it
                char new_relative[512];
                if (relative_path && strlen(relative_path) > 0) {
                    if (strlen(relative_path) + strlen(dir->d_name) + 2 > 512) continue;
                    snprintf(new_relative, sizeof(new_relative), "%s/%s", relative_path, dir->d_name);
                } else {
                    strncpy(new_relative, dir->d_name, sizeof(new_relative));
                    new_relative[sizeof(new_relative) - 1] = '\0';
                }
                scan_directory_recursive(base_path, new_relative, files, count, max_count);
            } else if (S_ISREG(statbuf.st_mode)) {
                // Regular file - check length before adding
                if (relative_path && strlen(relative_path) > 0) {
                    if (strlen(relative_path) + strlen(dir->d_name) + 2 > MAX_FILENAME) continue;
                    snprintf(files[*count], MAX_FILENAME, "%s/%s", relative_path, dir->d_name);
                } else {
                    if (strlen(dir->d_name) >= MAX_FILENAME) continue;
                    strncpy(files[*count], dir->d_name, MAX_FILENAME);
                    files[*count][MAX_FILENAME - 1] = '\0';
                }
                (*count)++;
            }
        }
    }
    closedir(d);
}
#pragma GCC diagnostic pop


// --- 8. Main Function ---
int main(int argc, char* argv[]) {
    // Command-line args: ./storage_server [port] [storage_path] [client_ip] [nm_ip]
    int my_port = MY_PORT_FOR_CLIENTS;
    char my_client_ip[MAX_IP_LEN] = MY_IP_FOR_CLIENTS;
    char nm_ip[MAX_IP_LEN] = "127.0.0.1";
    
    if (argc >= 2) my_port = atoi(argv[1]);
    if (argc >= 3) strncpy(g_storage_path, argv[2], sizeof(g_storage_path) - 1);
    if (argc >= 4) strncpy(my_client_ip, argv[3], sizeof(my_client_ip) - 1);
    if (argc >= 5) strncpy(nm_ip, argv[4], sizeof(nm_ip) - 1);
    
    ss_log_file = fopen("ss.log", "a"); if (ss_log_file == NULL) error_exit("fopen ss.log");
    log_event("--- Storage Server Started ---");
    log_event("Port: %d, Storage: %s, Client IP: %s, NM IP: %s", 
              my_port, g_storage_path, my_client_ip, nm_ip);
    
    int nm_sock, client_listener_sock, new_client_sock;
    struct sockaddr_in nm_addr, client_listen_addr, client_addr;
    socklen_t client_len; fd_set master_set, read_set; int fdmax;
    init_locks(); 
    for(int i = 0; i < MAX_CONNECTIONS; i++) { write_sessions[i].active = 0; write_sessions[i].virtual_word_count = 0; }
    for(int i = 0; i < MAX_FILES_IN_SYSTEM; i++) active_documents[i].active = 0; 
    
    char my_files[MAX_FILES_PER_SS][MAX_FILENAME]; int file_count = 0; mkdir(g_storage_path, 0777);
    log_event("Scanning storage directory: %s", g_storage_path);
    scan_directory_recursive(g_storage_path, "", my_files, &file_count, MAX_FILES_PER_SS);
    log_event("Found %d files.", file_count);
    nm_sock = socket(AF_INET, SOCK_STREAM, 0); if (nm_sock < 0) error_exit("socket");
    memset(&nm_addr, 0, sizeof(nm_addr)); nm_addr.sin_family = AF_INET; nm_addr.sin_port = htons(NM_PORT);
    if (inet_pton(AF_INET, nm_ip, &nm_addr.sin_addr) <= 0) error_exit("inet_pton");
    if (connect(nm_sock, (struct sockaddr *)&nm_addr, sizeof(nm_addr)) < 0) error_exit("connect");
    log_event("Storage Server connected to Name Server on port %d", NM_PORT);
    Header header; header.type = REQ_SS_REGISTER; header.payload_size = sizeof(Msg_SS_Register);
    Msg_SS_Register reg_msg; strncpy(reg_msg.ss_ip, my_client_ip, MAX_IP_LEN); reg_msg.client_port = my_port; reg_msg.file_count = file_count;
    if (send(nm_sock, &header, sizeof(Header), 0) < 0) error_exit("send header");
    if (send(nm_sock, &reg_msg, sizeof(reg_msg), 0) < 0) error_exit("send payload");
    log_event("Sending file list with metadata...");
    for (int i = 0; i < file_count; i++) {
        Msg_File_Item item; 
        strncpy(item.filename, my_files[i], MAX_FILENAME);
        load_file_metadata(my_files[i], item.owner, &item.access_count, item.access_list);
        log_event("  -> Sending '%s' (owner: %s, access_count: %d)", item.filename, item.owner, item.access_count);
        if (send(nm_sock, &item, sizeof(item), 0) < 0) error_exit("send file item"); 
    }
    if (recv(nm_sock, &header, sizeof(Header), 0) < 0 || header.type != RES_OK) error_exit("Registration failed");
    log_event("Registration successful!");
    log_event("Sending metadata for %d existing files...", file_count);
    for (int i = 0; i < file_count; i++) {
        char file_path[768];
        sprintf(file_path, "%s/%s", g_storage_path, my_files[i]);
        calculate_and_send_metadata(nm_sock, my_files[i], file_path);
    }
    client_listener_sock = socket(AF_INET, SOCK_STREAM, 0); if (client_listener_sock < 0) error_exit("client listener socket");
    int yes = 1; if (setsockopt(client_listener_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) < 0) error_exit("setsockopt");
    memset(&client_listen_addr, 0, sizeof(client_listen_addr)); client_listen_addr.sin_family = AF_INET; client_listen_addr.sin_addr.s_addr = INADDR_ANY; client_listen_addr.sin_port = htons(my_port);
    if (bind(client_listener_sock, (struct sockaddr *)&client_listen_addr, sizeof(client_listen_addr)) < 0) error_exit("client listener bind");
    if (listen(client_listener_sock, 10) < 0) error_exit("client listener listen");
    log_event("Storage Server now listening for clients on port %d...", my_port);
    FD_ZERO(&master_set); FD_SET(nm_sock, &master_set); FD_SET(client_listener_sock, &master_set);
    fdmax = (nm_sock > client_listener_sock) ? nm_sock : client_listener_sock;

    while (1) {
        read_set = master_set;
        if (select(fdmax + 1, &read_set, NULL, NULL, NULL) < 0) { log_event("select() error"); error_exit("select"); }
        for (int sock_fd = 0; sock_fd <= fdmax; sock_fd++) {
            if (FD_ISSET(sock_fd, &read_set)) {
                if (sock_fd == nm_sock) {
                    if (recv(nm_sock, &header, sizeof(Header), 0) <= 0) { log_event("Name Server disconnected!"); error_exit("Name Server disconnected"); }
                    switch (header.type) {
                        case REQ_SS_CREATE: {
                            Msg_SS_Create_Request req; recv(nm_sock, &req, sizeof(req), 0);
                            log_event("Got REQ_SS_CREATE for '%s' (owner: %s) from NM", req.filename, req.owner);
                            char file_path[256];
                            handle_create_file(req.filename, file_path);
                            AccessEntry empty_list[MAX_PERMISSIONS_PER_FILE];
                            save_file_metadata(req.filename, req.owner, 0, empty_list);
                            send_simple_header(nm_sock, RES_OK);
                            calculate_and_send_metadata(nm_sock, req.filename, file_path);
                            break;
                        }
                        case REQ_SS_DELETE: {
                            Msg_Filename_Request req; recv(nm_sock, &req, sizeof(req), 0);
                            log_event("Got REQ_SS_DELETE for '%s' from NM", req.filename);
                            char file_path[256]; sprintf(file_path, "%s/%s", g_storage_path, req.filename);
                            if (remove(file_path) == 0) { log_event("  -> File deleted."); } else { log_event("  -> Error deleting file: %s", strerror(errno)); }
                            char backup_path[256]; sprintf(backup_path, "%s/%s.bak", g_storage_path, req.filename); remove(backup_path);
                            delete_file_metadata(req.filename);
                            send_simple_header(nm_sock, RES_OK);
                            break;
                        }
                        case REQ_SS_UNDO: {
                            Msg_Filename_Request req; recv(nm_sock, &req, sizeof(req), 0);
                            log_event("Got REQ_SS_UNDO for '%s' from NM", req.filename);
                            
                            // Check if there's an __UNDO__ checkpoint from a recent revert
                            char undo_checkpoint[1024], redo_checkpoint[1024];
                            sprintf(undo_checkpoint, "%s/.checkpoints/%s/__UNDO__", g_storage_path, req.filename);
                            sprintf(redo_checkpoint, "%s/.checkpoints/%s/__REDO__", g_storage_path, req.filename);
                            
                            struct stat st;
                            if (stat(undo_checkpoint, &st) == 0) {
                                // Undo a revert operation - restore from __UNDO__ and save current to __REDO__
                                log_event("  -> Found __UNDO__ checkpoint, restoring pre-revert state");
                                char file_path[512];
                                sprintf(file_path, "%s/%s", g_storage_path, req.filename);
                                
                                // First save current state to __REDO__ for toggling back
                                FILE *current = fopen(file_path, "r");
                                FILE *redo = fopen(redo_checkpoint, "w");
                                if (current && redo) {
                                    char buffer[4096];
                                    size_t bytes;
                                    while ((bytes = fread(buffer, 1, sizeof(buffer), current)) > 0) {
                                        fwrite(buffer, 1, bytes, redo);
                                    }
                                    fclose(current);
                                    fclose(redo);
                                }
                                
                                // Now restore from __UNDO__
                                FILE *src = fopen(undo_checkpoint, "r");
                                FILE *dst = fopen(file_path, "w");
                                if (src && dst) {
                                    char buffer[4096];
                                    size_t bytes;
                                    while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {
                                        fwrite(buffer, 1, bytes, dst);
                                    }
                                    fclose(src);
                                    fclose(dst);
                                    
                                    // Update backup
                                    char backup_path[520];
                                    sprintf(backup_path, "%s.bak", file_path);
                                    FILE *bak = fopen(undo_checkpoint, "r");
                                    FILE *bak_dst = fopen(backup_path, "w");
                                    if (bak && bak_dst) {
                                        while ((bytes = fread(buffer, 1, sizeof(buffer), bak)) > 0) {
                                            fwrite(buffer, 1, bytes, bak_dst);
                                        }
                                        fclose(bak);
                                        fclose(bak_dst);
                                    }
                                    
                                    // Delete __UNDO__, keep __REDO__ for next toggle
                                    unlink(undo_checkpoint);
                                    log_event("  -> Restored from __UNDO__ checkpoint");
                                    send_simple_header(nm_sock, RES_OK);
                                } else {
                                    if (src) fclose(src);
                                    if (dst) fclose(dst);
                                    log_event("  -> Error restoring from __UNDO__: %s", strerror(errno));
                                    send_simple_header(nm_sock, RES_ERROR);
                                }
                                char original_path[256];
                                sprintf(original_path, "%s/%s", g_storage_path, req.filename);
                                calculate_and_send_metadata(nm_sock, req.filename, original_path);
                            } else if (stat(redo_checkpoint, &st) == 0) {
                                // Redo - toggle back to reverted state
                                log_event("  -> Found __REDO__ checkpoint, toggling back to reverted state");
                                char file_path[512];
                                sprintf(file_path, "%s/%s", g_storage_path, req.filename);
                                
                                // Save current to __UNDO__ for toggling back again
                                FILE *current = fopen(file_path, "r");
                                FILE *undo = fopen(undo_checkpoint, "w");
                                if (current && undo) {
                                    char buffer[4096];
                                    size_t bytes;
                                    while ((bytes = fread(buffer, 1, sizeof(buffer), current)) > 0) {
                                        fwrite(buffer, 1, bytes, undo);
                                    }
                                    fclose(current);
                                    fclose(undo);
                                }
                                
                                // Restore from __REDO__
                                FILE *src = fopen(redo_checkpoint, "r");
                                FILE *dst = fopen(file_path, "w");
                                if (src && dst) {
                                    char buffer[4096];
                                    size_t bytes;
                                    while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {
                                        fwrite(buffer, 1, bytes, dst);
                                    }
                                    fclose(src);
                                    fclose(dst);
                                    
                                    // Update backup
                                    char backup_path[520];
                                    sprintf(backup_path, "%s.bak", file_path);
                                    FILE *bak = fopen(redo_checkpoint, "r");
                                    FILE *bak_dst = fopen(backup_path, "w");
                                    if (bak && bak_dst) {
                                        while ((bytes = fread(buffer, 1, sizeof(buffer), bak)) > 0) {
                                            fwrite(buffer, 1, bytes, bak_dst);
                                        }
                                        fclose(bak);
                                        fclose(bak_dst);
                                    }
                                    
                                    // Delete __REDO__, keep __UNDO__ for next toggle
                                    unlink(redo_checkpoint);
                                    log_event("  -> Restored from __REDO__ checkpoint");
                                    send_simple_header(nm_sock, RES_OK);
                                } else {
                                    if (src) fclose(src);
                                    if (dst) fclose(dst);
                                    log_event("  -> Error restoring from __REDO__: %s", strerror(errno));
                                    send_simple_header(nm_sock, RES_ERROR);
                                }
                                char original_path[256];
                                sprintf(original_path, "%s/%s", g_storage_path, req.filename);
                                calculate_and_send_metadata(nm_sock, req.filename, original_path);
                            } else {
                                // No __UNDO__ or __REDO__ checkpoint, perform regular undo (swap with backup)
                                char original_path[256], backup_path[256], temp_swap_path[256];
                                sprintf(original_path, "%s/%s", g_storage_path, req.filename);
                                sprintf(backup_path, "%s/%s.bak", g_storage_path, req.filename);
                                sprintf(temp_swap_path, "%s/%s.swap", g_storage_path, req.filename);
                                rename(original_path, temp_swap_path); rename(backup_path, original_path); rename(temp_swap_path, backup_path);
                                log_event("  -> Swapped backup file.");
                                send_simple_header(nm_sock, RES_OK);
                                calculate_and_send_metadata(nm_sock, req.filename, original_path);
                            }
                            break;
                        }
                        case REQ_SS_ADD_ACCESS: {
                            Msg_Access_Request req; recv(nm_sock, &req, sizeof(req), 0);
                            log_event("Got REQ_SS_ADD_ACCESS for '%s' (user: %s, perm: %d)", req.filename, req.username, req.perm);
                            char owner[MAX_USERNAME];
                            int access_count;
                            AccessEntry access_list[MAX_PERMISSIONS_PER_FILE];
                            load_file_metadata(req.filename, owner, &access_count, access_list);
                            if (access_count < MAX_PERMISSIONS_PER_FILE) {
                                strncpy(access_list[access_count].username, req.username, MAX_USERNAME);
                                access_list[access_count].permission = req.perm;
                                access_count++;
                                save_file_metadata(req.filename, owner, access_count, access_list);
                            }
                            break;
                        }
                        case REQ_SS_REM_ACCESS: {
                            Msg_Access_Request req; recv(nm_sock, &req, sizeof(req), 0);
                            log_event("Got REQ_SS_REM_ACCESS for '%s' (user: %s)", req.filename, req.username);
                            char owner[MAX_USERNAME];
                            int access_count;
                            AccessEntry access_list[MAX_PERMISSIONS_PER_FILE];
                            load_file_metadata(req.filename, owner, &access_count, access_list);
                            int found_idx = -1;
                            for (int i = 0; i < access_count; i++) {
                                if (strcmp(access_list[i].username, req.username) == 0) {
                                    found_idx = i;
                                    break;
                                }
                            }
                            if (found_idx != -1) {
                                for (int i = found_idx; i < access_count - 1; i++) {
                                    access_list[i] = access_list[i + 1];
                                }
                                access_count--;
                                save_file_metadata(req.filename, owner, access_count, access_list);
                            }
                            break;
                        }
                        case REQ_SS_CREATEFOLDER: {
                            Msg_Folder_Request req; recv(nm_sock, &req, sizeof(req), 0);
                            log_event("Got REQ_SS_CREATEFOLDER for '%s' from NM", req.foldername);
                            char folder_path[768];
                            sprintf(folder_path, "%s/%s", g_storage_path, req.foldername);
                            
                            // First check if folder already exists
                            struct stat st;
                            if (stat(folder_path, &st) == 0 && S_ISDIR(st.st_mode)) {
                                log_event("  -> Folder already exists: %s", folder_path);
                                send_simple_header(nm_sock, RES_ERROR);
                                break;
                            }
                            
                            // Create parent directories recursively (mkdir -p style)
                            char temp_path[512];
                            strncpy(temp_path, folder_path, sizeof(temp_path));
                            temp_path[sizeof(temp_path) - 1] = '\0';
                            
                            int success = 1;
                            for (char* p = temp_path + strlen(g_storage_path) + 1; *p; p++) {
                                if (*p == '/') {
                                    *p = '\0';
                                    if (mkdir(temp_path, 0777) != 0 && errno != EEXIST) {
                                        success = 0;
                                        break;
                                    }
                                    *p = '/';
                                }
                            }
                            
                            // Create final directory
                            if (success && mkdir(folder_path, 0777) != 0) {
                                success = 0;
                            }
                            
                            if (success) {
                                log_event("  -> Folder created at: %s", folder_path);
                                send_simple_header(nm_sock, RES_OK);
                            } else {
                                log_event("  -> Error creating folder: %s", strerror(errno));
                                send_simple_header(nm_sock, RES_ERROR);
                            }
                            break;
                        }
                        case REQ_SS_CHECKFOLDER: {
                            Msg_Folder_Request req; recv(nm_sock, &req, sizeof(req), 0);
                            log_event("Got REQ_SS_CHECKFOLDER for '%s' from NM", req.foldername);
                            char folder_path[512];
                            sprintf(folder_path, "%s/%s", g_storage_path, req.foldername);
                            
                            struct stat st;
                            if (stat(folder_path, &st) == 0 && S_ISDIR(st.st_mode)) {
                                log_event("  -> Folder exists: %s", folder_path);
                                send_simple_header(nm_sock, RES_OK);
                            } else {
                                log_event("  -> Folder not found: %s", folder_path);
                                send_simple_header(nm_sock, RES_ERROR);
                            }
                            break;
                        }
                        case REQ_SS_MOVE: {
                            Msg_Move_Request req; recv(nm_sock, &req, sizeof(req), 0);
                            log_event("Got REQ_SS_MOVE for '%s' to folder '%s' from NM", req.filename, req.foldername);
                            
                            // Extract just the base filename (in case it has a folder path)
                            char* base_filename = strrchr(req.filename, '/');
                            if (base_filename) {
                                base_filename++; // Skip the '/'
                            } else {
                                base_filename = req.filename; // No folder, use as-is
                            }
                            
                            char old_path[512], new_path[512], folder_path[512];
                            sprintf(old_path, "%s/%s", g_storage_path, req.filename);
                            sprintf(folder_path, "%s/%s", g_storage_path, req.foldername);
                            sprintf(new_path, "%s/%s/%s", g_storage_path, req.foldername, base_filename);
                            
                            // Ensure folder exists
                            mkdir(folder_path, 0777);
                            
                            // Move file
                            if (rename(old_path, new_path) == 0) {
                                log_event("  -> File moved from '%s' to '%s'", old_path, new_path);
                                
                                // Also move backup file if it exists
                                char old_backup[520], new_backup[520];
                                sprintf(old_backup, "%s.bak", old_path);
                                sprintf(new_backup, "%s.bak", new_path);
                                rename(old_backup, new_backup);
                                
                                // Update metadata file path
                                char old_meta_name[MAX_FILENAME], new_meta_name[MAX_FILENAME * 2];
                                strncpy(old_meta_name, req.filename, MAX_FILENAME);
                                snprintf(new_meta_name, sizeof(new_meta_name), "%s/%s", req.foldername, base_filename);
                                
                                char owner[MAX_USERNAME];
                                int access_count;
                                AccessEntry access_list[MAX_PERMISSIONS_PER_FILE];
                                load_file_metadata(old_meta_name, owner, &access_count, access_list);
                                delete_file_metadata(old_meta_name);
                                save_file_metadata(new_meta_name, owner, access_count, access_list);
                                
                                send_simple_header(nm_sock, RES_OK);
                            } else {
                                log_event("  -> Error moving file: %s", strerror(errno));
                                send_simple_header(nm_sock, RES_ERROR);
                            }
                            break;
                        }
                        case REQ_SS_CHECKPOINT: {
                            Msg_Checkpoint_Request req; recv(nm_sock, &req, sizeof(req), 0);
                            log_event("Got REQ_SS_CHECKPOINT for '%s' with tag '%s' from NM", req.filename, req.tag);
                            
                            char file_path[512], checkpoint_dir[512], checkpoint_path[1024];
                            sprintf(file_path, "%s/%s", g_storage_path, req.filename);
                            sprintf(checkpoint_dir, "%s/.checkpoints/%s", g_storage_path, req.filename);
                            sprintf(checkpoint_path, "%s/%s", checkpoint_dir, req.tag);
                            
                            // Check if file exists
                            struct stat st;
                            if (stat(file_path, &st) != 0) {
                                log_event("  -> Error: File '%s' not found", req.filename);
                                send_simple_header(nm_sock, RES_ERROR);
                                break;
                            }
                            
                            // Check if checkpoint with this tag already exists
                            if (stat(checkpoint_path, &st) == 0) {
                                log_event("  -> Error: Checkpoint tag '%s' already exists", req.tag);
                                send_simple_header(nm_sock, RES_ERROR);
                                break;
                            }
                            
                            // Create checkpoint directory (recursively)
                            char temp_dir[512];
                            strncpy(temp_dir, checkpoint_dir, sizeof(temp_dir));
                            temp_dir[sizeof(temp_dir) - 1] = '\0';
                            for (char* p = temp_dir + strlen(g_storage_path) + 1; *p; p++) {
                                if (*p == '/') {
                                    *p = '\0';
                                    mkdir(temp_dir, 0777);
                                    *p = '/';
                                }
                            }
                            mkdir(checkpoint_dir, 0777);
                            
                            // Copy file to checkpoint
                            FILE *src = fopen(file_path, "r");
                            FILE *dst = fopen(checkpoint_path, "w");
                            if (src && dst) {
                                char buffer[4096];
                                size_t bytes;
                                while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {
                                    fwrite(buffer, 1, bytes, dst);
                                }
                                fclose(src);
                                fclose(dst);
                                log_event("  -> Checkpoint '%s' created for '%s'", req.tag, req.filename);
                                send_simple_header(nm_sock, RES_OK);
                            } else {
                                if (src) fclose(src);
                                if (dst) fclose(dst);
                                log_event("  -> Error creating checkpoint: %s", strerror(errno));
                                send_simple_header(nm_sock, RES_ERROR);
                            }
                            break;
                        }
                        case REQ_SS_VIEWCHECKPOINT: {
                            Msg_Checkpoint_Request req; recv(nm_sock, &req, sizeof(req), 0);
                            log_event("Got REQ_SS_VIEWCHECKPOINT for '%s' tag '%s' from NM", req.filename, req.tag);
                            
                            char checkpoint_path[1024];
                            sprintf(checkpoint_path, "%s/.checkpoints/%s/%s", g_storage_path, req.filename, req.tag);
                            
                            struct stat st;
                            if (stat(checkpoint_path, &st) != 0) {
                                log_event("  -> Error: Checkpoint '%s' not found for file '%s'", req.tag, req.filename);
                                send_simple_header(nm_sock, RES_ERROR);
                                break;
                            }
                            
                            // Send file content
                            FILE *f = fopen(checkpoint_path, "r");
                            if (f) {
                                fseek(f, 0, SEEK_END);
                                long size = ftell(f);
                                fseek(f, 0, SEEK_SET);
                                
                                Header res_hdr;
                                res_hdr.type = RES_SS_FILE_OK;
                                res_hdr.payload_size = size;
                                send(nm_sock, &res_hdr, sizeof(res_hdr), 0);
                                
                                char buffer[4096];
                                size_t bytes;
                                while ((bytes = fread(buffer, 1, sizeof(buffer), f)) > 0) {
                                    send(nm_sock, buffer, bytes, 0);
                                }
                                fclose(f);
                                log_event("  -> Sent checkpoint content (%ld bytes)", size);
                            } else {
                                log_event("  -> Error opening checkpoint: %s", strerror(errno));
                                send_simple_header(nm_sock, RES_ERROR);
                            }
                            break;
                        }
                        case REQ_SS_REVERT: {
                            Msg_Checkpoint_Request req; recv(nm_sock, &req, sizeof(req), 0);
                            log_event("Got REQ_SS_REVERT for '%s' to tag '%s' from NM", req.filename, req.tag);
                            
                            char file_path[512], checkpoint_path[1024], undo_path[1024];
                            sprintf(file_path, "%s/%s", g_storage_path, req.filename);
                            sprintf(checkpoint_path, "%s/.checkpoints/%s/%s", g_storage_path, req.filename, req.tag);
                            sprintf(undo_path, "%s/.checkpoints/%s/__UNDO__", g_storage_path, req.filename);
                            
                            // Check if checkpoint exists
                            struct stat st;
                            if (stat(checkpoint_path, &st) != 0) {
                                log_event("  -> Error: Checkpoint '%s' not found for file '%s'", req.tag, req.filename);
                                send_simple_header(nm_sock, RES_ERROR);
                                break;
                            }
                            
                            // First, save current state to __UNDO__ checkpoint for undo functionality
                            FILE *current_file = fopen(file_path, "r");
                            FILE *undo_file = fopen(undo_path, "w");
                            if (current_file && undo_file) {
                                char buffer[4096];
                                size_t bytes;
                                while ((bytes = fread(buffer, 1, sizeof(buffer), current_file)) > 0) {
                                    fwrite(buffer, 1, bytes, undo_file);
                                }
                                fclose(current_file);
                                fclose(undo_file);
                                log_event("  -> Saved current state to __UNDO__ checkpoint");
                            } else {
                                if (current_file) fclose(current_file);
                                if (undo_file) fclose(undo_file);
                            }
                            
                            // Now copy checkpoint back to main file
                            FILE *src = fopen(checkpoint_path, "r");
                            FILE *dst = fopen(file_path, "w");
                            if (src && dst) {
                                char buffer[4096];
                                size_t bytes;
                                while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {
                                    fwrite(buffer, 1, bytes, dst);
                                }
                                fclose(src);
                                fclose(dst);
                                
                                // Also update backup
                                char backup_path[520];
                                sprintf(backup_path, "%s.bak", file_path);
                                FILE *bak = fopen(checkpoint_path, "r");
                                FILE *bak_dst = fopen(backup_path, "w");
                                if (bak && bak_dst) {
                                    while ((bytes = fread(buffer, 1, sizeof(buffer), bak)) > 0) {
                                        fwrite(buffer, 1, bytes, bak_dst);
                                    }
                                    fclose(bak);
                                    fclose(bak_dst);
                                }
                                
                                log_event("  -> File '%s' reverted to checkpoint '%s'", req.filename, req.tag);
                                send_simple_header(nm_sock, RES_OK);
                            } else {
                                if (src) fclose(src);
                                if (dst) fclose(dst);
                                log_event("  -> Error reverting file: %s", strerror(errno));
                                send_simple_header(nm_sock, RES_ERROR);
                            }
                            break;
                        }
                        case REQ_SS_LISTCHECKPOINTS: {
                            Msg_ListCheckpoints_Request req; recv(nm_sock, &req, sizeof(req), 0);
                            log_event("Got REQ_SS_LISTCHECKPOINTS for '%s' from NM", req.filename);
                            
                            char checkpoint_dir[512];
                            sprintf(checkpoint_dir, "%s/.checkpoints/%s", g_storage_path, req.filename);
                            
                            // Count checkpoints
                            int count = 0;
                            Msg_Checkpoint_Item items[MAX_FILES_PER_SS];
                            
                            DIR *d = opendir(checkpoint_dir);
                            if (d) {
                                struct dirent *dir;
                                while ((dir = readdir(d)) != NULL && count < MAX_FILES_PER_SS) {
                                    if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0) continue;
                                    if (strcmp(dir->d_name, "__UNDO__") == 0) continue; // Skip internal undo checkpoint
                                    if (strcmp(dir->d_name, "__REDO__") == 0) continue; // Skip internal redo checkpoint
                                    
                                    char checkpoint_path[1024];
                                    sprintf(checkpoint_path, "%s/%s", checkpoint_dir, dir->d_name);
                                    
                                    struct stat st;
                                    if (stat(checkpoint_path, &st) == 0 && S_ISREG(st.st_mode)) {
                                        strncpy(items[count].tag, dir->d_name, MAX_CHECKPOINT_TAG);
                                        items[count].tag[MAX_CHECKPOINT_TAG - 1] = '\0';
                                        items[count].timestamp = st.st_mtime;
                                        count++;
                                    }
                                }
                                closedir(d);
                            }
                            
                            // Send response
                            Header res_hdr;
                            res_hdr.type = RES_CHECKPOINT_LIST;
                            res_hdr.payload_size = sizeof(Msg_Checkpoint_List_Hdr);
                            send(nm_sock, &res_hdr, sizeof(res_hdr), 0);
                            
                            Msg_Checkpoint_List_Hdr hdr;
                            hdr.checkpoint_count = count;
                            send(nm_sock, &hdr, sizeof(hdr), 0);
                            
                            for (int i = 0; i < count; i++) {
                                send(nm_sock, &items[i], sizeof(Msg_Checkpoint_Item), 0);
                            }
                            
                            log_event("  -> Sent %d checkpoint(s) for '%s'", count, req.filename);
                            break;
                        }
                        case REQ_REPLICATE_FILE: {
                            Msg_Replicate_File rep_msg; 
                            recv(nm_sock, &rep_msg, sizeof(rep_msg), 0);
                            log_event("Got REQ_REPLICATE_FILE for '%s' (backup copy)", rep_msg.filename);
                            
                            // Save metadata
                            save_file_metadata(rep_msg.filename, rep_msg.owner, rep_msg.access_count, rep_msg.access_list);
                            
                            // Create empty file (actual content will come via async channel or ignored)
                            char file_path[768];
                            sprintf(file_path, "%s/%s", g_storage_path, rep_msg.filename);
                            int fd = open(file_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                            if (fd >= 0) {
                                close(fd);
                                log_event("  -> Backup file '%s' created", rep_msg.filename);
                            }
                            break;
                        }
                        case REQ_SS_HEARTBEAT: {
                            // Respond immediately
                            Header hb_res;
                            hb_res.type = RES_SS_HEARTBEAT;
                            hb_res.payload_size = 0;
                            send(nm_sock, &hb_res, sizeof(hb_res), 0);
                            break;
                        }
                        case REQ_GET_FILE_CONTENT: {
                            Msg_Sync_File sync_msg;
                            recv(nm_sock, &sync_msg, sizeof(sync_msg), 0);
                            log_event("Got REQ_GET_FILE_CONTENT for '%s' (backup restore)", sync_msg.filename);
                            
                            char file_path[768];
                            sprintf(file_path, "%s/%s", g_storage_path, sync_msg.filename);
                            
                            FILE* file = fopen(file_path, "rb");
                            if (!file) {
                                long zero = 0;
                                send(nm_sock, &zero, sizeof(long), 0);
                                log_event("  -> File not found");
                                break;
                            }
                            
                            fseek(file, 0, SEEK_END);
                            long file_size = ftell(file);
                            fseek(file, 0, SEEK_SET);
                            
                            send(nm_sock, &file_size, sizeof(long), 0);
                            
                            char buffer[4096];
                            int bytes_read;
                            while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
                                send(nm_sock, buffer, bytes_read, 0);
                            }
                            fclose(file);
                            log_event("  -> Sent %ld bytes", file_size);
                            break;
                        }
                        case REQ_SYNC_FROM_BACKUP: {
                            Msg_Sync_File sync_msg;
                            recv(nm_sock, &sync_msg, sizeof(sync_msg), 0);
                            
                            long file_size;
                            recv(nm_sock, &file_size, sizeof(long), 0);
                            
                            log_event("Got REQ_SYNC_FROM_BACKUP for '%s' (%ld bytes)", 
                                     sync_msg.filename, file_size);
                            
                            char file_path[768];
                            sprintf(file_path, "%s/%s", g_storage_path, sync_msg.filename);
                            
                            FILE* file = fopen(file_path, "wb");
                            if (!file) {
                                log_event("  -> Error creating file");
                                break;
                            }
                            
                            char* content = malloc(file_size);
                            if (content) {
                                int received = 0;
                                while (received < file_size) {
                                    int bytes = recv(nm_sock, content + received, file_size - received, 0);
                                    if (bytes <= 0) break;
                                    received += bytes;
                                }
                                fwrite(content, 1, received, file);
                                free(content);
                                log_event("  -> File restored (%d bytes)", received);
                            }
                            fclose(file);
                            break;
                        }
                        default:
                            log_event("Got unknown command %d from NM", header.type);
                    }
                }
                else if (sock_fd == client_listener_sock) {
                    client_len = sizeof(client_addr);
                    new_client_sock = accept(client_listener_sock, (struct sockaddr *)&client_addr, &client_len);
                    if (new_client_sock < 0) { perror("accept new client"); }
                    else {
                        FD_SET(new_client_sock, &master_set);
                        if (new_client_sock > fdmax) fdmax = new_client_sock;
                        log_event("New client connection from %s on socket %d", inet_ntoa(client_addr.sin_addr), new_client_sock);
                    }
                }
                else {
                    if (recv(sock_fd, &header, sizeof(Header), 0) <= 0) {
                        handle_client_disconnect(sock_fd, &master_set);
                    } else {
                        switch (header.type) {
                            case REQ_CLIENT_READ: {
                                Msg_Filename_Request req; recv(sock_fd, &req, sizeof(req), 0);
                                log_event("Got REQ_CLIENT_READ for '%s' from socket %d", req.filename, sock_fd);
                                handle_send_file(sock_fd, req.filename);
                                close(sock_fd); FD_CLR(sock_fd, &master_set);
                                break;
                            }
                            case REQ_CLIENT_STREAM: {
                                Msg_Filename_Request req; recv(sock_fd, &req, sizeof(req), 0);
                                log_event("Got REQ_CLIENT_STREAM for '%s' from socket %d", req.filename, sock_fd);
                                handle_stream_file(sock_fd, req.filename);
                                close(sock_fd); FD_CLR(sock_fd, &master_set);
                                break;
                            }
                            case REQ_CLIENT_WRITE: {
                                Msg_Client_Write req; recv(sock_fd, &req, sizeof(req), 0);
                                log_event("Got REQ_CLIENT_WRITE for '%s' (sent %d) from socket %d", req.filename, req.sentence_num, sock_fd);
                                
                                ActiveDoc* doc = find_or_load_active_doc(req.filename);
                                if(doc == NULL) {
                                    log_event("  -> ERROR: Failed to load document for writing.");
                                    send_simple_header(sock_fd, RES_ERROR);
                                    close(sock_fd); FD_CLR(sock_fd, &master_set);
                                    break;
                                }
                                // Get sentence pointer by index
                                SentenceNode* target = doc->doc_head;
                                for (int i = 0; i < req.sentence_num && target != NULL; i++) target = target->next;
                                
                                if (target == NULL) {
                                    log_event("  -> ERROR: Sentence index %d out of bounds.", req.sentence_num);
                                    send_simple_header(sock_fd, RES_ERROR_INVALID_SENTENCE);
                                    // Don't call release_active_doc() - we never incremented num_users_editing
                                    close(sock_fd); FD_CLR(sock_fd, &master_set);
                                    break;
                                }
                                
                                if (find_lock(req.filename, target) != -1) {
                                    log_event("  -> Lock conflict! Sending error.");
                                    send_simple_header(sock_fd, RES_ERROR_LOCKED);
                                    // Don't call release_active_doc() - we never incremented num_users_editing
                                    close(sock_fd); FD_CLR(sock_fd, &master_set);
                                } else {
                                    // Record original word count at session start
                                    int original_word_count = 0;
                                    WordNode* w = target->word_head;
                                    while (w != NULL) { original_word_count++; w = w->next; }
                                    
                                    create_lock(req.filename, target, sock_fd);
                                    doc->num_users_editing++;
                                    WriteSession* session = &write_sessions[sock_fd];
                                    session->active = 1;
                                    session->doc_index = doc - active_documents;
                                    session->sentence_ptr = target;
                                    session->original_word_count = original_word_count;
                                    session->edit_capacity = 100;
                                    session->edit_ops = (EditOperation*)malloc(session->edit_capacity * sizeof(EditOperation));
                                    session->edit_count = 0;
                                    session->virtual_word_count = 0;
                                    
                                    log_event("  -> Lock granted. Session started. Original word count: %d, Users editing: %d", 
                                             original_word_count, doc->num_users_editing);
                                    send_simple_header(sock_fd, RES_OK_LOCKED);
                                }
                                break;
                            }
                            case REQ_WRITE_UPDATE: {
                                if (!write_sessions[sock_fd].active) { log_event("Error: Got REQ_WRITE_UPDATE from socket %d with no active session.", sock_fd); break; }
                                WriteSession* session = &write_sessions[sock_fd];
                                Msg_Write_Update req; recv(sock_fd, &req, sizeof(req), 0);
                                
                                // Validate word index against current sentence state
                                SentenceNode* target = session->sentence_ptr;
                                if (target == NULL) {
                                    log_event("  -> ERROR: Locked sentence pointer is NULL");
                                    Header response_header;
                                    response_header.type = RES_ERROR;
                                    response_header.payload_size = 0;
                                    send(sock_fd, &response_header, sizeof(response_header), 0);
                                    break;
                                }
                                
                                // Count words in current sentence
                                int current_word_count = 0;
                                WordNode* w = target->word_head;
                                while (w != NULL) { current_word_count++; w = w->next; }
                                
                                // Add virtual word count to allow indexing into pending edits
                                int total_word_count = current_word_count + session->virtual_word_count;
                                
                                // Validate word index (0-based indexing, can insert at position 0 to total_word_count)
                                if (req.word_index < 0 || req.word_index > total_word_count) {
                                    log_event("  -> ERROR: Word index %d out of bounds (valid range: 0-%d)", 
                                             req.word_index, total_word_count);
                                    Header response_header;
                                    response_header.type = RES_ERROR;
                                    response_header.payload_size = 0;
                                    send(sock_fd, &response_header, sizeof(response_header), 0);
                                    break;
                                }
                                
                                // Record the operation
                                if (session->edit_count >= session->edit_capacity) {
                                    session->edit_capacity *= 2;
                                    session->edit_ops = (EditOperation*)realloc(session->edit_ops, 
                                                                               session->edit_capacity * sizeof(EditOperation));
                                }
                                session->edit_ops[session->edit_count].word_index = req.word_index;
                                strncpy(session->edit_ops[session->edit_count].content, req.content, sizeof(session->edit_ops[0].content) - 1);
                                session->edit_ops[session->edit_count].content[sizeof(session->edit_ops[0].content) - 1] = '\0';
                                // Track virtual word count (sum of tokens sent before ETIRW)
                                int added_words = count_words_in_string(req.content);
                                session->virtual_word_count += added_words;
                                session->edit_count++;
                                
                                log_event("  -> Recorded edit operation #%d (word %d) for socket %d", 
                                         session->edit_count, req.word_index, sock_fd);
                                
                                Header response_header;
                                response_header.type = RES_OK;
                                response_header.payload_size = 0;
                                send(sock_fd, &response_header, sizeof(response_header), 0);
                                break;
                            }
                            case REQ_ETIRW: {
                                if (!write_sessions[sock_fd].active) { log_event("Error: Got REQ_ETIRW from socket %d with no active session.", sock_fd); break; }
                                WriteSession* session = &write_sessions[sock_fd];
                                ActiveDoc* doc = &active_documents[session->doc_index];
                                log_event("Got REQ_ETIRW from socket %d. Committing %d edit operations for '%s'.", 
                                         sock_fd, session->edit_count, doc->filename);
                                
                                // The locked sentence pointer is STILL VALID in the current doc
                                // Calculate its current word count and apply edits directly
                                SentenceNode* target = session->sentence_ptr;
                                
                                if (target == NULL) {
                                    log_event("  -> ERROR: Locked sentence pointer is NULL");
                                    send_simple_header(sock_fd, RES_ERROR);
                                    break;
                                }
                                
                                // Calculate current word count and offset
                                int current_word_count = 0;
                                WordNode* w = target->word_head;
                                while (w != NULL) { current_word_count++; w = w->next; }
                                
                                int word_offset = current_word_count - session->original_word_count;
                                log_event("  -> Original word count: %d, Current: %d, Offset: %d", 
                                         session->original_word_count, current_word_count, word_offset);
                                
                                // Find the sentence index for handle_write_update_list
                                int sentence_idx = 0;
                                SentenceNode* temp = doc->doc_head;
                                while (temp != NULL && temp != target) {
                                    sentence_idx++;
                                    temp = temp->next;
                                }
                                
                                if (temp == NULL) {
                                    log_event("  -> ERROR: Could not find locked sentence in document");
                                    send_simple_header(sock_fd, RES_ERROR);
                                    break;
                                }
                                
                                // Apply all edit operations atomically
                                int validation_failed = 0;

                                // compute doc word count before applying edits
                                int doc_words_before = 0;
                                SentenceNode* tmp = doc->doc_head;
                                while (tmp) { WordNode* tw = tmp->word_head; while (tw) { doc_words_before++; tw = tw->next; } tmp = tmp->next; }

                                // The word_offset represents how many words were added to the locked sentence
                                // by OTHER concurrent editors. We need to offset only the FIRST edit by this amount.
                                // Subsequent edits from this client are cumulative (each builds on the previous).
                                //
                                // However, since apply_session_edits_to_sentence applies edits in order and
                                // each edit shifts subsequent positions, we only need to offset the FIRST edit's index.
                                // Actually, we need to think about this differently...
                                //
                                // The client sent edits with indices 0, 1, 2, etc. relative to their view.
                                // If the sentence grew from other edits, ALL positions shift by word_offset.
                                // But WITHIN this client's edits, they're cumulative.
                                //
                                // Solution: Just offset the first edit, since apply_session_edits_to_sentence
                                // handles cumulative application correctly.
                                
                                if (session->edit_count > 0 && word_offset != 0) {
                                    // Only adjust the first edit - subsequent edits are cumulative
                                    session->edit_ops[0].word_index += word_offset;
                                    log_event("  -> Adjusted first edit index by offset %d", word_offset);
                                }

                                // Apply edits atomically (this will split sentences only after all inserts)
                                if (apply_session_edits_to_sentence(doc, target, session) != 0) {
                                    log_event("  -> ERROR: Failed to apply session edits atomically");
                                    send_simple_header(sock_fd, RES_ERROR);
                                    validation_failed = 1;
                                }

                                if (!validation_failed) {
                                    // compute how many words were actually added to the document
                                    int doc_words_after = 0;
                                    tmp = doc->doc_head;
                                    while (tmp) { WordNode* tw = tmp->word_head; while (tw) { doc_words_after++; tw = tw->next; } tmp = tmp->next; }
                                    int added = doc_words_after - doc_words_before;
                                    if (added != session->virtual_word_count) {
                                        log_event("  -> ERROR: Virtual added words (%d) != actual added (%d). Rejecting.", session->virtual_word_count, added);
                                        send_simple_header(sock_fd, RES_ERROR_INVALID_SENTENCE);
                                        validation_failed = 1;
                                    } else {
                                        // Commit changes to file
                                        rename(doc->original_path, doc->backup_path);
                                        flush_list_to_file(doc->doc_head, doc->original_path);
                                    }
                                }
                                
                                // Clean up session
                                release_lock(doc->filename, session->sentence_ptr);
                                free(session->edit_ops);
                                session->edit_ops = NULL;
                                session->active = 0;
                                release_active_doc(doc);
                                
                                calculate_and_send_metadata(nm_sock, doc->filename, doc->original_path);
                                send_simple_header(sock_fd, RES_OK);
                                log_event("  -> Commit successful for socket %d", sock_fd);
                                close(sock_fd);
                                FD_CLR(sock_fd, &master_set);
                                break;
                            }
                            default:
                                log_event("Got unknown command %d from client %d", header.type, sock_fd);
                        }
                    }
                }
            }
        }
    } 
    return 0;
}