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
typedef struct { int active; char filename[MAX_FILENAME]; int sentence_num; int sock_fd; } LockInfo;
LockInfo global_locks[MAX_LOCKS]; 

typedef struct {
    int active;
    char filename[MAX_FILENAME];
    SentenceNode* doc_head; 
    int num_users_editing;  
    char original_path[256];
    char backup_path[256];
} ActiveDoc;
ActiveDoc active_documents[MAX_FILES_IN_SYSTEM];

typedef struct {
    int active;
    int doc_index;    
    int sentence_num; 
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

// --- 4. Lock Helpers (Same as before) ---
void init_locks() { for (int i = 0; i < MAX_LOCKS; i++) global_locks[i].active = 0; }
int find_lock(char* f, int s) { for (int i = 0; i < MAX_LOCKS; i++) if (global_locks[i].active && !strcmp(global_locks[i].filename, f) && global_locks[i].sentence_num == s) return i; return -1; }
int create_lock(char* f, int s, int sock) { if (find_lock(f, s) != -1) return 0; for (int i = 0; i < MAX_LOCKS; i++) if (!global_locks[i].active) { global_locks[i].active = 1; strncpy(global_locks[i].filename, f, MAX_FILENAME); global_locks[i].sentence_num = s; global_locks[i].sock_fd = sock; log_event("  -> Lock CREATED for '%s' (sent %d) by socket %d", f, s, sock); return 1; } return -1; }
void release_lock(char* f, int s) { int i = find_lock(f, s); if (i != -1) { global_locks[i].active = 0; log_event("  -> Lock RELEASED for '%s' (sent %d)", f, s); } }

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
    
    sprintf(doc->original_path, "%s/%s", MY_STORAGE_PATH, filename);
    sprintf(doc->backup_path, "%s/%s.bak", MY_STORAGE_PATH, filename);
    
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
    sprintf(file_path, "%s/%s", MY_STORAGE_PATH, filename);
    log_event("  -> Creating file at: %s", file_path);
    FILE* f = fopen(file_path, "w");
    if (f) { fclose(f); }
}
void handle_send_file(int client_sock, char* filename) { char file_path[256]; sprintf(file_path, "%s/%s", MY_STORAGE_PATH, filename); int fd = open(file_path, O_RDONLY); if (fd < 0) { log_event("  -> File not found. Sending error to socket %d", client_sock); send_simple_header(client_sock, RES_ERROR_NOT_FOUND); return; } send_simple_header(client_sock, RES_SS_FILE_OK); log_event("  -> Sending file '%s' to socket %d", filename, client_sock); char buffer[FILE_BUFFER_SIZE]; int bytes_read; while ((bytes_read = read(fd, buffer, FILE_BUFFER_SIZE)) > 0) if (send(client_sock, buffer, bytes_read, 0) < 0) break; close(fd); log_event("  -> Finished sending file to socket %d", client_sock); }
void handle_stream_file(int client_sock, char* filename) {
    char file_path[256]; sprintf(file_path, "%s/%s", MY_STORAGE_PATH, filename);
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
        
        release_lock(doc->filename, session->sentence_num);
        release_active_doc(doc); 
        session->active = 0;
    }
    close(sock_fd);
    FD_CLR(sock_fd, master_set);
}


// --- 8. Main Function ---
int main() {
    ss_log_file = fopen("ss.log", "a"); if (ss_log_file == NULL) error_exit("fopen ss.log");
    log_event("--- Storage Server Started ---");
    int nm_sock, client_listener_sock, new_client_sock;
    struct sockaddr_in nm_addr, client_listen_addr, client_addr;
    socklen_t client_len; fd_set master_set, read_set; int fdmax;
    init_locks(); 
    for(int i = 0; i < MAX_CONNECTIONS; i++) write_sessions[i].active = 0;
    for(int i = 0; i < MAX_FILES_IN_SYSTEM; i++) active_documents[i].active = 0; 
    
    char my_files[MAX_FILES_PER_SS][MAX_FILENAME]; int file_count = 0; mkdir(MY_STORAGE_PATH, 0777);
    log_event("Scanning storage directory: %s", MY_STORAGE_PATH);
    DIR *d = opendir(MY_STORAGE_PATH);
    if (d) { struct dirent *dir; while ((dir = readdir(d)) != NULL && file_count < MAX_FILES_PER_SS) { if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0) continue; strncpy(my_files[file_count], dir->d_name, MAX_FILENAME); file_count++; } closedir(d); }
    log_event("Found %d files.", file_count);
    nm_sock = socket(AF_INET, SOCK_STREAM, 0); if (nm_sock < 0) error_exit("socket");
    memset(&nm_addr, 0, sizeof(nm_addr)); nm_addr.sin_family = AF_INET; nm_addr.sin_port = htons(NM_PORT);
    if (inet_pton(AF_INET, "127.0.0.1", &nm_addr.sin_addr) <= 0) error_exit("inet_pton");
    if (connect(nm_sock, (struct sockaddr *)&nm_addr, sizeof(nm_addr)) < 0) error_exit("connect");
    log_event("Storage Server connected to Name Server on port %d", NM_PORT);
    Header header; header.type = REQ_SS_REGISTER; header.payload_size = sizeof(Msg_SS_Register);
    Msg_SS_Register reg_msg; strncpy(reg_msg.ss_ip, MY_IP_FOR_CLIENTS, MAX_IP_LEN); reg_msg.client_port = MY_PORT_FOR_CLIENTS; reg_msg.file_count = file_count;
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
        char file_path[256];
        sprintf(file_path, "%s/%s", MY_STORAGE_PATH, my_files[i]);
        calculate_and_send_metadata(nm_sock, my_files[i], file_path);
    }
    client_listener_sock = socket(AF_INET, SOCK_STREAM, 0); if (client_listener_sock < 0) error_exit("client listener socket");
    int yes = 1; if (setsockopt(client_listener_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) < 0) error_exit("setsockopt");
    memset(&client_listen_addr, 0, sizeof(client_listen_addr)); client_listen_addr.sin_family = AF_INET; client_listen_addr.sin_addr.s_addr = INADDR_ANY; client_listen_addr.sin_port = htons(MY_PORT_FOR_CLIENTS);
    if (bind(client_listener_sock, (struct sockaddr *)&client_listen_addr, sizeof(client_listen_addr)) < 0) error_exit("client listener bind");
    if (listen(client_listener_sock, 10) < 0) error_exit("client listener listen");
    log_event("Storage Server now listening for clients on port %d...", MY_PORT_FOR_CLIENTS);
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
                            char file_path[256]; sprintf(file_path, "%s/%s", MY_STORAGE_PATH, req.filename);
                            if (remove(file_path) == 0) { log_event("  -> File deleted."); } else { log_event("  -> Error deleting file: %s", strerror(errno)); }
                            char backup_path[256]; sprintf(backup_path, "%s/%s.bak", MY_STORAGE_PATH, req.filename); remove(backup_path);
                            delete_file_metadata(req.filename);
                            send_simple_header(nm_sock, RES_OK);
                            break;
                        }
                        case REQ_SS_UNDO: {
                            Msg_Filename_Request req; recv(nm_sock, &req, sizeof(req), 0);
                            log_event("Got REQ_SS_UNDO for '%s' from NM", req.filename);
                            char original_path[256], backup_path[256], temp_swap_path[256];
                            sprintf(original_path, "%s/%s", MY_STORAGE_PATH, req.filename);
                            sprintf(backup_path, "%s/%s.bak", MY_STORAGE_PATH, req.filename);
                            sprintf(temp_swap_path, "%s/%s.swap", MY_STORAGE_PATH, req.filename);
                            rename(original_path, temp_swap_path); rename(backup_path, original_path); rename(temp_swap_path, backup_path);
                            log_event("  -> Swapped backup file.");
                            send_simple_header(nm_sock, RES_OK);
                            calculate_and_send_metadata(nm_sock, req.filename, original_path);
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
                                
                                if (find_lock(req.filename, req.sentence_num) != -1) {
                                    log_event("  -> Lock conflict! Sending error.");
                                    send_simple_header(sock_fd, RES_ERROR_LOCKED);
                                    close(sock_fd); FD_CLR(sock_fd, &master_set);
                                } else {
                                    ActiveDoc* doc = find_or_load_active_doc(req.filename);
                                    if(doc == NULL) {
                                        log_event("  -> ERROR: Failed to load document for writing.");
                                        send_simple_header(sock_fd, RES_ERROR);
                                        close(sock_fd); FD_CLR(sock_fd, &master_set);
                                        break;
                                    }
                                    // Validate sentence index
                                    SentenceNode* temp = doc->doc_head;
                                    int sent_count = 0;
                                    while (temp != NULL) { sent_count++; temp = temp->next; }
                                    if (req.sentence_num < 0 || req.sentence_num >= sent_count) {
                                        log_event("  -> ERROR: Sentence index %d out of bounds (0-%d).", req.sentence_num, sent_count - 1);
                                        send_simple_header(sock_fd, RES_ERROR_INVALID_SENTENCE);
                                        release_active_doc(doc);
                                        close(sock_fd); FD_CLR(sock_fd, &master_set);
                                        break;
                                    }
                                    create_lock(req.filename, req.sentence_num, sock_fd);
                                    doc->num_users_editing++;
                                    WriteSession* session = &write_sessions[sock_fd];
                                    session->active = 1;
                                    session->doc_index = doc - active_documents;
                                    session->sentence_num = req.sentence_num;
                                    log_event("  -> Lock granted. Session started. Users editing: %d", doc->num_users_editing);
                                    send_simple_header(sock_fd, RES_OK_LOCKED);
                                }
                                break;
                            }
                            case REQ_WRITE_UPDATE: {
                                if (!write_sessions[sock_fd].active) { log_event("Error: Got REQ_WRITE_UPDATE from socket %d with no active session.", sock_fd); break; }
                                WriteSession* session = &write_sessions[sock_fd];
                                ActiveDoc* doc = &active_documents[session->doc_index];
                                Msg_Write_Update req; recv(sock_fd, &req, sizeof(req), 0);
                                
                                // Validate word index
                                SentenceNode* target = doc->doc_head;
                                for (int i = 0; i < session->sentence_num && target != NULL; i++) target = target->next;
                                int valid = 1;
                                if (target != NULL) {
                                    int word_count = 0;
                                    WordNode* w = target->word_head;
                                    while (w != NULL) { word_count++; w = w->next; }
                                    if (req.word_index < 0 || req.word_index > word_count) {
                                        log_event("  -> ERROR: Word index %d out of bounds (0-%d) for socket %d", req.word_index, word_count, sock_fd);
                                        valid = 0;
                                    }
                                }
                                
                                Header response_header;
                                if (valid) {
                                    char* content_copy = strdup(req.content); // Need to copy, strtok modifies
                                    handle_write_update_list(doc->doc_head, session->sentence_num, req.word_index, content_copy);
                                    free(content_copy); // Free the copy
                                    log_event("  -> Applied update (word %d) to in-memory list for socket %d", req.word_index, sock_fd);
                                    response_header.type = RES_OK;
                                } else {
                                    response_header.type = RES_ERROR_INVALID_WORD;
                                }
                                response_header.payload_size = 0;
                                send(sock_fd, &response_header, sizeof(response_header), 0);
                                break;
                            }
                            case REQ_ETIRW: {
                                if (!write_sessions[sock_fd].active) { log_event("Error: Got REQ_ETIRW from socket %d with no active session.", sock_fd); break; }
                                WriteSession* session = &write_sessions[sock_fd];
                                ActiveDoc* doc = &active_documents[session->doc_index];
                                log_event("Got REQ_ETIRW from socket %d. Committing changes for '%s'.", sock_fd, doc->filename);
                                
                                rename(doc->original_path, doc->backup_path);
                                flush_list_to_file(doc->doc_head, doc->original_path);
                                release_lock(doc->filename, session->sentence_num);
                                session->active = 0;
                                release_active_doc(doc); 
                                calculate_and_send_metadata(nm_sock, doc->filename, doc->original_path);
                                send_simple_header(sock_fd, RES_OK);
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