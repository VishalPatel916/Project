#include "protocol.h"
#include <sys/select.h>
#include <netinet/in.h>
#include <time.h>
#include <stdarg.h>

// --- Global Log File ---
FILE* nm_log_file;

// --- 1. NM's Internal Data Structures (C-Style) ---
#define MAX_CONNECTIONS FD_SETSIZE
typedef struct { int active; char username[MAX_USERNAME]; char ip_addr[MAX_IP_LEN]; } ClientInfo;
typedef struct { int active; char ip[MAX_IP_LEN]; int client_port; char ip_addr[MAX_IP_LEN]; } SSInfo;
typedef struct {
    int active;
    char filename[MAX_FILENAME];
    int ss_sock_fd; 
    char owner[MAX_USERNAME];
    AccessEntry access_list[MAX_PERMISSIONS_PER_FILE];
    int access_count;
    long file_size;
    int word_count;
    int char_count;
    time_t last_modified;
} FileMetadata;

// --- 2. The NM's Global State ---
ClientInfo client_state[MAX_CONNECTIONS];
SSInfo ss_state[MAX_CONNECTIONS];
FileMetadata file_catalog[MAX_FILES_IN_SYSTEM];

// --- 3. EFFICIENT SEARCH STRUCTURES ---
#define HASH_SIZE 1024 // Hash table size (power of 2)
#define CACHE_SIZE 5   // LRU cache for recent lookups

// Hash Map Node for O(1) lookup
typedef struct HashNode {
    char key[MAX_FILENAME];
    int slot_index; // Index in file_catalog
    struct HashNode* next; // Collision handling via chaining
} HashNode;

HashNode* hash_table[HASH_SIZE];

// LRU Cache Entry
typedef struct {
    int valid;
    char key[MAX_FILENAME];
    int slot_index;
} CacheEntry;

CacheEntry lru_cache[CACHE_SIZE];

// --- 4. Logging Function ---
void log_event(const char* format, ...) {
    char time_buf[50]; time_t now = time(NULL); strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", localtime(&now));
    va_list args;
    printf("[%s] ", time_buf); va_start(args, format); vprintf(format, args); va_end(args); printf("\n");
    fprintf(nm_log_file, "[%s] ", time_buf); va_start(args, format); vfprintf(nm_log_file, format, args); va_end(args); fprintf(nm_log_file, "\n");
    fflush(nm_log_file);
}

// --- 5. HASH MAP & CACHE FUNCTIONS ---

// Simple djb2 hash function
unsigned long hash_func(char *str) {
    unsigned long hash = 5381;
    int c;
    while ((c = *str++)) hash = ((hash << 5) + hash) + c;
    return hash % HASH_SIZE;
}

void add_to_cache(char* filename, int slot_index) {
    // Check if already in cache
    for(int i=0; i<CACHE_SIZE; i++) {
        if(lru_cache[i].valid && strcmp(lru_cache[i].key, filename) == 0) return;
    }
    // Shift right (evict last)
    for(int i=CACHE_SIZE-1; i > 0; i--) {
        lru_cache[i] = lru_cache[i-1];
    }
    // Insert at front
    strcpy(lru_cache[0].key, filename);
    lru_cache[0].slot_index = slot_index;
    lru_cache[0].valid = 1;
}

void invalidate_cache(char* filename) {
    for(int i=0; i<CACHE_SIZE; i++) {
        if(lru_cache[i].valid && strcmp(lru_cache[i].key, filename) == 0) {
            lru_cache[i].valid = 0;
        }
    }
}

void add_to_hashmap(char* filename, int slot_index) {
    unsigned long idx = hash_func(filename);
    HashNode* new_node = (HashNode*)malloc(sizeof(HashNode));
    strcpy(new_node->key, filename);
    new_node->slot_index = slot_index;
    new_node->next = hash_table[idx]; // Insert at head
    hash_table[idx] = new_node;
}

void remove_from_hashmap(char* filename) {
    unsigned long idx = hash_func(filename);
    HashNode* current = hash_table[idx];
    HashNode* prev = NULL;
    while(current != NULL) {
        if(strcmp(current->key, filename) == 0) {
            if(prev == NULL) hash_table[idx] = current->next;
            else prev->next = current->next;
            free(current);
            return;
        }
        prev = current;
        current = current->next;
    }
}

// --- 6. Helper Functions ---
// O(1) Find Function using Cache + Hash Map
int find_file_slot(char* filename) {
    // 1. Check Cache (Fastest)
    for(int i=0; i<CACHE_SIZE; i++) {
        if(lru_cache[i].valid && strcmp(lru_cache[i].key, filename) == 0) {
            log_event("[CACHE HIT] '%s' found in cache at position %d", filename, i);
            return lru_cache[i].slot_index;
        }
    }

    // 2. Check Hash Map (Fast)
    unsigned long idx = hash_func(filename);
    HashNode* node = hash_table[idx];
    while(node != NULL) {
        if(strcmp(node->key, filename) == 0) {
            // Found! Update cache and return
            log_event("[HASH HIT] '%s' found in hash map, adding to cache", filename);
            add_to_cache(filename, node->slot_index);
            return node->slot_index;
        }
        node = node->next;
    }

    log_event("[MISS] '%s' not found in cache or hash map", filename);
    return -1; // Not found
}
int find_empty_file_slot() { for (int i = 0; i < MAX_FILES_IN_SYSTEM; i++) { if (file_catalog[i].active == 0) return i; } return -1; }
int find_available_ss() { for (int i = 0; i < MAX_CONNECTIONS; i++) { if (ss_state[i].active) return i; } return -1; }
void send_ok_response(int sock) { Header header; header.type = RES_OK; header.payload_size = 0; if (send(sock, &header, sizeof(Header), 0) < 0) { log_event("Failed to send OK response to socket %d", sock); } }
void handle_disconnect(int sock_fd) {
    if (client_state[sock_fd].active) {
        log_event("Client '%s' (Socket %d, IP: %s) disconnected.", client_state[sock_fd].username, sock_fd, client_state[sock_fd].ip_addr);
        client_state[sock_fd].active = 0;
    } else if (ss_state[sock_fd].active) {
        log_event("Storage Server (Socket %d, IP: %s) disconnected.", sock_fd, ss_state[sock_fd].ip_addr);
        ss_state[sock_fd].active = 0;
        log_event("De-listing its files from the catalog...");
        for (int i = 0; i < MAX_FILES_IN_SYSTEM; i++) {
            if (file_catalog[i].active && file_catalog[i].ss_sock_fd == sock_fd) {
                // REMOVE FROM HASHMAP & CACHE
                remove_from_hashmap(file_catalog[i].filename);
                invalidate_cache(file_catalog[i].filename);
                
                file_catalog[i].active = 0;
                log_event("  -> De-listed '%s'", file_catalog[i].filename);
            }
        }
    } else { log_event("Socket %d (unregistered) disconnected.", sock_fd); }
    close(sock_fd);
}

PermissionLevel get_permission(FileMetadata* meta, char* username) {
    if (strcmp(meta->owner, username) == 0) { return READ_WRITE; }
    for (int i = 0; i < meta->access_count; i++) {
        if (strcmp(meta->access_list[i].username, username) == 0) {
            return meta->access_list[i].permission;
        }
    }
    return NO_PERM;
}
int has_read_access(FileMetadata* meta, char* username) { return get_permission(meta, username) >= READ_ONLY; }
int has_write_access(FileMetadata* meta, char* username) { return get_permission(meta, username) == READ_WRITE; }

void send_full_metadata(int sock_fd, FileMetadata* meta) {
    Header hdr; hdr.payload_size = sizeof(Msg_Full_Metadata);
    Msg_Full_Metadata msg;
    strncpy(msg.filename, meta->filename, MAX_FILENAME);
    strncpy(msg.owner, meta->owner, MAX_USERNAME);
    msg.file_size = meta->file_size; msg.word_count = meta->word_count; msg.char_count = meta->char_count;
    msg.last_modified = meta->last_modified; msg.access_count = meta->access_count;
    if (meta->active == 1) { hdr.type = RES_VIEW_ITEM_LONG; meta->active = 2; }
    else { hdr.type = RES_INFO; }
    send(sock_fd, &hdr, sizeof(hdr), 0); send(sock_fd, &msg, sizeof(msg), 0);
    if (msg.access_count > 0) { send(sock_fd, meta->access_list, sizeof(AccessEntry) * msg.access_count, 0); }
}

// --- NEW HELPER: NM acts as a client to get file content from SS ---
// Returns a malloc'd buffer with the file content, or NULL on failure.
// Note: This is a simple implementation, assumes script is not huge.
char* get_file_content_from_ss(SSInfo* ss, char* filename) {
    int ss_sock;
    struct sockaddr_in ss_addr;
    
    // 1. Connect to the SS's CLIENT port
    ss_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (ss_sock < 0) { log_event("  -> EXEC: socket() failed"); return NULL; }

    memset(&ss_addr, 0, sizeof(ss_addr));
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(ss->client_port);
    if (inet_pton(AF_INET, ss->ip, &ss_addr.sin_addr) <= 0) {
        log_event("  -> EXEC: inet_pton() failed"); close(ss_sock); return NULL;
    }
    if (connect(ss_sock, (struct sockaddr *)&ss_addr, sizeof(ss_addr)) < 0) {
        log_event("  -> EXEC: connect() to SS failed"); close(ss_sock); return NULL;
    }

    // 2. Send a normal REQ_CLIENT_READ
    Header req_hdr;
    req_hdr.type = REQ_CLIENT_READ;
    req_hdr.payload_size = sizeof(Msg_Filename_Request);
    Msg_Filename_Request req_msg;
    strncpy(req_msg.filename, filename, MAX_FILENAME);
    send(ss_sock, &req_hdr, sizeof(req_hdr), 0);
    send(ss_sock, &req_msg, sizeof(req_msg), 0);
    
    // 3. Wait for handshake
    Header res_hdr;
    if (recv(ss_sock, &res_hdr, sizeof(res_hdr), 0) <= 0 || res_hdr.type != RES_SS_FILE_OK) {
        log_event("  -> EXEC: SS did not send RES_SS_FILE_OK");
        close(ss_sock); return NULL;
    }

    // 4. Malloc a buffer and receive the file content
    // Warning: This is a simple implementation. A robust one would
    // realloc() as needed. 16KB max script size.
    int max_script_size = 16384;
    char* script_content = (char*)malloc(max_script_size);
    if (script_content == NULL) { close(ss_sock); return NULL; }
    
    int total_bytes = 0;
    int nbytes = 0;
    while(total_bytes < max_script_size - 1 &&
          (nbytes = recv(ss_sock, script_content + total_bytes, max_script_size - 1 - total_bytes, 0)) > 0) {
        total_bytes += nbytes;
    }
    script_content[total_bytes] = '\0'; // Null-terminate the script
    
    close(ss_sock);
    log_event("  -> EXEC: Successfully fetched script content (%d bytes).", total_bytes);
    return script_content;
}


int main() {
    int listener_sock, new_sock, sock_fd;
    int fdmax; struct sockaddr_in nm_addr, client_addr; socklen_t client_len;
    fd_set master_set, read_set;

    nm_log_file = fopen("nm.log", "a"); if (nm_log_file == NULL) error_exit("fopen nm.log");
    log_event("--- Name Server Started ---");
    log_event("Initializing state...");
    for (int i = 0; i < MAX_CONNECTIONS; i++) { client_state[i].active = 0; ss_state[i].active = 0; }
    for (int i = 0; i < MAX_FILES_IN_SYSTEM; i++) { file_catalog[i].active = 0; file_catalog[i].access_count = 0; }
    
    // Initialize Hash Map and LRU Cache
    for(int i=0; i<HASH_SIZE; i++) hash_table[i] = NULL;
    for(int i=0; i<CACHE_SIZE; i++) lru_cache[i].valid = 0;
    log_event("Hash map and LRU cache initialized.");

    listener_sock = socket(AF_INET, SOCK_STREAM, 0); if (listener_sock < 0) error_exit("socket");
    int yes = 1; if (setsockopt(listener_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) < 0) error_exit("setsockopt");
    memset(&nm_addr, 0, sizeof(nm_addr)); nm_addr.sin_family = AF_INET; nm_addr.sin_addr.s_addr = INADDR_ANY; nm_addr.sin_port = htons(NM_PORT);
    if (bind(listener_sock, (struct sockaddr *)&nm_addr, sizeof(nm_addr)) < 0) error_exit("bind");
    if (listen(listener_sock, 10) < 0) error_exit("listen");

    FD_ZERO(&master_set); FD_SET(listener_sock, &master_set);
    fdmax = listener_sock;
    log_event("Name Server listening on port %d...", NM_PORT);

    while (1) {
        read_set = master_set;
        if (select(fdmax + 1, &read_set, NULL, NULL, NULL) < 0) { log_event("select() error"); error_exit("select"); }
        for (sock_fd = 0; sock_fd <= fdmax; sock_fd++) {
            if (FD_ISSET(sock_fd, &read_set)) {
                
                if (sock_fd == listener_sock) {
                    client_len = sizeof(client_addr);
                    new_sock = accept(listener_sock, (struct sockaddr *)&client_addr, &client_len);
                    if (new_sock < 0) { perror("accept"); }
                    else {
                        FD_SET(new_sock, &master_set); if (new_sock > fdmax) fdmax = new_sock;
                        log_event("New connection from %s on socket %d", inet_ntoa(client_addr.sin_addr), new_sock);
                    }
                } 
                else {
                    Header header; int nbytes = recv(sock_fd, &header, sizeof(Header), 0);
                    if (nbytes <= 0) { handle_disconnect(sock_fd); FD_CLR(sock_fd, &master_set); } 
                    else {
                        struct sockaddr_in addr; socklen_t len = sizeof(addr);
                        getpeername(sock_fd, (struct sockaddr*)&addr, &len); char* peer_ip = inet_ntoa(addr.sin_addr);
                        switch (header.type) {
                            case REQ_CLIENT_REGISTER: {
                                Msg_Client_Register msg; recv(sock_fd, &msg, sizeof(msg), 0);
                                log_event("Socket %d (%s): CLIENT registered as '%s'", sock_fd, peer_ip, msg.username);
                                client_state[sock_fd].active = 1; strncpy(client_state[sock_fd].username, msg.username, MAX_USERNAME);
                                strncpy(client_state[sock_fd].ip_addr, peer_ip, MAX_IP_LEN);
                                ss_state[sock_fd].active = 0;
                                send_ok_response(sock_fd);
                                break;
                            }
                            case REQ_SS_REGISTER: {
                                Msg_SS_Register msg; recv(sock_fd, &msg, sizeof(msg), 0);
                                log_event("Socket %d (%s): SS registered at %s:%d. Expecting %d files.", sock_fd, peer_ip, msg.ss_ip, msg.client_port, msg.file_count);
                                ss_state[sock_fd].active = 1; strncpy(ss_state[sock_fd].ip, msg.ss_ip, MAX_IP_LEN);
                                ss_state[sock_fd].client_port = msg.client_port; strncpy(ss_state[sock_fd].ip_addr, peer_ip, MAX_IP_LEN);
                                client_state[sock_fd].active = 0;
                                log_event("Receiving file list from SS %d...", sock_fd);
                                for (int i = 0; i < msg.file_count; i++) {
                                    Msg_File_Item item; recv(sock_fd, &item, sizeof(item), 0);
                                    int slot = find_empty_file_slot();
                                    if (slot != -1) {
                                        file_catalog[slot].active = 1; 
                                        strncpy(file_catalog[slot].filename, item.filename, MAX_FILENAME);
                                        file_catalog[slot].ss_sock_fd = sock_fd; 
                                        strncpy(file_catalog[slot].owner, item.owner, MAX_USERNAME);
                                        file_catalog[slot].access_count = item.access_count;
                                        for (int j = 0; j < item.access_count; j++) {
                                            file_catalog[slot].access_list[j] = item.access_list[j];
                                        }
                                        // ADD TO HASHMAP
                                        add_to_hashmap(item.filename, slot);
                                        log_event("  -> Cataloged '%s' (owner: %s, %d access entries) in slot %d", 
                                            item.filename, item.owner, item.access_count, slot);
                                    }
                                }
                                send_ok_response(sock_fd);
                                break;
                            }
                            case REQ_CREATE: {
                                Msg_Filename_Request req; recv(sock_fd, &req, sizeof(req), 0);
                                log_event("Got REQ_CREATE for '%s' from client '%s' (Sock %d)", req.filename, client_state[sock_fd].username, sock_fd);
                                if (find_file_slot(req.filename) != -1) { log_event("  -> Error: File '%s' already exists.", req.filename); send_simple_header(sock_fd, RES_ERROR_FILE_EXISTS);
                                } else {
                                    int ss_sock = find_available_ss();
                                    if (ss_sock == -1) { log_event("  -> Error: No Storage Servers available."); send_simple_header(sock_fd, RES_ERROR); }
                                    else {
                                        log_event("  -> Relaying REQ_SS_CREATE to SS on socket %d", ss_sock);
                                        Header ss_header; ss_header.type = REQ_SS_CREATE; ss_header.payload_size = sizeof(Msg_SS_Create_Request);
                                        Msg_SS_Create_Request ss_req;
                                        strncpy(ss_req.filename, req.filename, MAX_FILENAME);
                                        strncpy(ss_req.owner, client_state[sock_fd].username, MAX_USERNAME);
                                        send(ss_sock, &ss_header, sizeof(ss_header), 0); send(ss_sock, &ss_req, sizeof(ss_req), 0);
                                        recv(ss_sock, &header, sizeof(Header), 0); 
                                        if (header.type == RES_OK) {
                                            log_event("  -> SS confirmed creation. Updating catalog.");
                                            int slot = find_empty_file_slot();
                                            file_catalog[slot].active = 1; strncpy(file_catalog[slot].filename, req.filename, MAX_FILENAME);
                                            file_catalog[slot].ss_sock_fd = ss_sock; strncpy(file_catalog[slot].owner, client_state[sock_fd].username, MAX_USERNAME);
                                            file_catalog[slot].access_count = 0;
                                            // ADD TO HASHMAP
                                            add_to_hashmap(req.filename, slot);
                                            send_ok_response(sock_fd);
                                        } else { log_event("  -> SS failed to create file."); send_simple_header(sock_fd, RES_ERROR); }
                                    }
                                }
                                break;
                            }
                            case REQ_READ:
                            case REQ_WRITE:
                            case REQ_STREAM: {
                                Msg_Filename_Request req; recv(sock_fd, &req, sizeof(req), 0);
                                log_event("Got %d for '%s' from client '%s'", header.type, req.filename, client_state[sock_fd].username);
                                int slot = find_file_slot(req.filename);
                                if (slot == -1) { log_event("  -> Error: File not found."); send_simple_header(sock_fd, RES_ERROR_NOT_FOUND); break; }
                                FileMetadata* meta = &file_catalog[slot];
                                int has_perm = 0;
                                if(header.type == REQ_WRITE) has_perm = has_write_access(meta, client_state[sock_fd].username);
                                else has_perm = has_read_access(meta, client_state[sock_fd].username);
                                if (!has_perm) {
                                    log_event("  -> Access Denied for user '%s'.", client_state[sock_fd].username);
                                    send_simple_header(sock_fd, RES_ERROR_ACCESS_DENIED); break;
                                }
                                SSInfo* ss = &ss_state[meta->ss_sock_fd];
                                if (!ss->active) { log_event("  -> Error: SS for file is offline."); send_simple_header(sock_fd, RES_ERROR); }
                                else {
                                    log_event("  -> File found on SS %d (%s:%d)", meta->ss_sock_fd, ss->ip, ss->client_port);
                                    Header res_hdr; res_hdr.type = RES_READ_LOCATION; res_hdr.payload_size = sizeof(Msg_Read_Response);
                                    Msg_Read_Response res_payload; strncpy(res_payload.ss_ip, ss->ip, MAX_IP_LEN); res_payload.ss_port = ss->client_port;
                                    send(sock_fd, &res_hdr, sizeof(res_hdr), 0); send(sock_fd, &res_payload, sizeof(res_payload), 0);
                                }
                                break;
                            }
                            case REQ_LIST: {
                                log_event("Got REQ_LIST from client '%s' (Sock %d)", client_state[sock_fd].username, sock_fd);
                                int count = 0; for(int i = 0; i < MAX_CONNECTIONS; i++) if (client_state[i].active) count++;
                                Header res_hdr; res_hdr.type = RES_LIST_HDR; res_hdr.payload_size = sizeof(Msg_List_Hdr);
                                Msg_List_Hdr list_hdr; list_hdr.user_count = count;
                                send(sock_fd, &res_hdr, sizeof(res_hdr), 0); send(sock_fd, &list_hdr, sizeof(list_hdr), 0);
                                for(int i = 0; i < MAX_CONNECTIONS; i++) {
                                    if (client_state[i].active) { Msg_List_Item item; strncpy(item.username, client_state[i].username, MAX_USERNAME); send(sock_fd, &item, sizeof(item), 0); }
                                }
                                break;
                            }
                            case REQ_DELETE: {
                                Msg_Filename_Request req; recv(sock_fd, &req, sizeof(req), 0);
                                log_event("Got REQ_DELETE for '%s' from client '%s'", req.filename, client_state[sock_fd].username);
                                int slot = find_file_slot(req.filename);
                                if (slot == -1) { send_simple_header(sock_fd, RES_ERROR_NOT_FOUND); break; }
                                FileMetadata* meta = &file_catalog[slot];
                                if (strcmp(meta->owner, client_state[sock_fd].username) != 0) {
                                    log_event("  -> Access Denied. User '%s' is not owner '%s'.", client_state[sock_fd].username, meta->owner);
                                    send_simple_header(sock_fd, RES_ERROR_ACCESS_DENIED); break;
                                }
                                int ss_sock = meta->ss_sock_fd;
                                if (!ss_state[ss_sock].active) { send_simple_header(sock_fd, RES_ERROR); break; }
                                log_event("  -> Relaying REQ_SS_DELETE to SS %d", ss_sock);
                                Header ss_header; ss_header.type = REQ_SS_DELETE; ss_header.payload_size = sizeof(Msg_Filename_Request);
                                send(ss_sock, &ss_header, sizeof(ss_header), 0); send(ss_sock, &req, sizeof(req), 0);
                                recv(ss_sock, &header, sizeof(Header), 0);
                                
                                // REMOVE FROM HASHMAP & CACHE
                                remove_from_hashmap(file_catalog[slot].filename);
                                invalidate_cache(file_catalog[slot].filename);
                                
                                file_catalog[slot].active = 0;
                                send_ok_response(sock_fd);
                                break;
                            }
                            case REQ_UNDO: {
                                Msg_Filename_Request req; recv(sock_fd, &req, sizeof(req), 0);
                                log_event("Got REQ_UNDO for '%s' from client '%s'", req.filename, client_state[sock_fd].username);
                                int slot = find_file_slot(req.filename);
                                if (slot == -1) { send_simple_header(sock_fd, RES_ERROR_NOT_FOUND); break; }
                                FileMetadata* meta = &file_catalog[slot];
                                if (!has_write_access(meta, client_state[sock_fd].username)) {
                                    log_event("  -> Access Denied (UNDO). User '%s' does not have write access.", client_state[sock_fd].username);
                                    send_simple_header(sock_fd, RES_ERROR_ACCESS_DENIED); break;
                                }
                                int ss_sock = meta->ss_sock_fd;
                                if (!ss_state[ss_sock].active) { send_simple_header(sock_fd, RES_ERROR); break; }
                                log_event("  -> Relaying REQ_SS_UNDO to SS %d", ss_sock);
                                Header ss_header; ss_header.type = REQ_SS_UNDO; ss_header.payload_size = sizeof(Msg_Filename_Request);
                                send(ss_sock, &ss_header, sizeof(ss_header), 0); send(ss_sock, &req, sizeof(req), 0);
                                recv(ss_sock, &header, sizeof(Header), 0);
                                send_ok_response(sock_fd);
                                break;
                            }
                            case REQ_UPDATE_METADATA: {
                                Msg_Update_Metadata msg; recv(sock_fd, &msg, sizeof(msg), 0);
                                log_event("Got REQ_UPDATE_METADATA for '%s' from SS %d", msg.filename, sock_fd);
                                int slot = find_file_slot(msg.filename);
                                if (slot != -1) {
                                    file_catalog[slot].file_size = msg.file_size;
                                    file_catalog[slot].word_count = msg.word_count;
                                    file_catalog[slot].char_count = msg.char_count;
                                    file_catalog[slot].last_modified = msg.last_modified;
                                    log_event("  -> Metadata updated for '%s'", msg.filename);
                                } else { log_event("  -> ERROR: Received metadata for unknown file '%s'", msg.filename); }
                                break;
                            }
                            case REQ_VIEW: {
                                Msg_View_Request req; recv(sock_fd, &req, sizeof(req), 0);
                                log_event("Got REQ_VIEW (a=%d, l=%d) from client '%s'", req.flag_a, req.flag_l, client_state[sock_fd].username);
                                int count = 0;
                                for(int i=0; i < MAX_FILES_IN_SYSTEM; i++) {
                                    if (!file_catalog[i].active) continue;
                                    if (req.flag_a || has_read_access(&file_catalog[i], client_state[sock_fd].username)) {
                                        count++;
                                    }
                                }
                                Header res_hdr; res_hdr.type = RES_VIEW_HDR; res_hdr.payload_size = sizeof(Msg_View_Hdr);
                                Msg_View_Hdr view_hdr; view_hdr.file_count = count;
                                send(sock_fd, &res_hdr, sizeof(res_hdr), 0); send(sock_fd, &view_hdr, sizeof(view_hdr), 0);
                                for(int i=0; i < MAX_FILES_IN_SYSTEM; i++) {
                                    if (!file_catalog[i].active) continue;
                                    if (req.flag_a || has_read_access(&file_catalog[i], client_state[sock_fd].username)) {
                                        if (req.flag_l) {
                                            file_catalog[i].active = 1;
                                            send_full_metadata(sock_fd, &file_catalog[i]);
                                        } else {
                                            res_hdr.type = RES_VIEW_ITEM_SHORT; res_hdr.payload_size = sizeof(Msg_View_Item_Short);
                                            Msg_View_Item_Short item; strncpy(item.filename, file_catalog[i].filename, MAX_FILENAME);
                                            send(sock_fd, &res_hdr, sizeof(res_hdr), 0); send(sock_fd, &item, sizeof(item), 0);
                                        }
                                    }
                                }
                                break;
                            }
                            case REQ_INFO: {
                                Msg_Filename_Request req; recv(sock_fd, &req, sizeof(req), 0);
                                log_event("Got REQ_INFO for '%s' from client '%s'", req.filename, client_state[sock_fd].username);
                                int slot = find_file_slot(req.filename);
                                if (slot == -1) { send_simple_header(sock_fd, RES_ERROR_NOT_FOUND); break; }
                                FileMetadata* meta = &file_catalog[slot];
                                // INFO is public - no permission check needed (like ls -al)
                                meta->active = 0; send_full_metadata(sock_fd, meta); meta->active = 1;
                                break;
                            }
                            case REQ_ADD_ACCESS: {
                                Msg_Access_Request req; recv(sock_fd, &req, sizeof(req), 0);
                                log_event("Got REQ_ADD_ACCESS for '%s' (user: %s, perm: %d) from client '%s'", req.filename, req.username, req.perm, client_state[sock_fd].username);
                                int slot = find_file_slot(req.filename);
                                if (slot == -1) { send_simple_header(sock_fd, RES_ERROR_NOT_FOUND); break; }
                                FileMetadata* meta = &file_catalog[slot];
                                if (strcmp(meta->owner, client_state[sock_fd].username) != 0) {
                                    send_simple_header(sock_fd, RES_ERROR_ACCESS_DENIED); break;
                                }
                                
                                // Check if user already has an entry - update it instead of adding duplicate
                                int existing_idx = -1;
                                for (int i = 0; i < meta->access_count; i++) {
                                    if (strcmp(meta->access_list[i].username, req.username) == 0) {
                                        existing_idx = i;
                                        break;
                                    }
                                }
                                
                                if (existing_idx != -1) {
                                    // Update existing entry
                                    meta->access_list[existing_idx].permission = req.perm;
                                    log_event("  -> Updated existing access for user '%s'.", req.username);
                                } else {
                                    // Add new entry
                                    if (meta->access_count >= MAX_PERMISSIONS_PER_FILE) {
                                        send_simple_header(sock_fd, RES_ERROR); break;
                                    }
                                    AccessEntry* new_entry = &meta->access_list[meta->access_count];
                                    strncpy(new_entry->username, req.username, MAX_USERNAME);
                                    new_entry->permission = req.perm;
                                    meta->access_count++;
                                    log_event("  -> Access granted to new user '%s'.", req.username);
                                }
                                
                                // Relay to SS for persistence
                                int ss_sock = meta->ss_sock_fd;
                                if (ss_state[ss_sock].active) {
                                    Header ss_header; ss_header.type = REQ_SS_ADD_ACCESS; ss_header.payload_size = sizeof(Msg_Access_Request);
                                    send(ss_sock, &ss_header, sizeof(ss_header), 0);
                                    send(ss_sock, &req, sizeof(req), 0);
                                }
                                
                                send_ok_response(sock_fd);
                                break;
                            }
                            case REQ_REM_ACCESS: {
                                Msg_Access_Request req; recv(sock_fd, &req, sizeof(req), 0);
                                log_event("Got REQ_REM_ACCESS for '%s' (user: %s) from client '%s'", req.filename, req.username, client_state[sock_fd].username);
                                int slot = find_file_slot(req.filename);
                                if (slot == -1) { send_simple_header(sock_fd, RES_ERROR_NOT_FOUND); break; }
                                FileMetadata* meta = &file_catalog[slot];
                                if (strcmp(meta->owner, client_state[sock_fd].username) != 0) {
                                    send_simple_header(sock_fd, RES_ERROR_ACCESS_DENIED); break;
                                }
                                
                                // Remove ALL entries for this user (in case of duplicates)
                                int removed_count = 0;
                                for(int i = 0; i < meta->access_count; ) {
                                    if(strcmp(meta->access_list[i].username, req.username) == 0) {
                                        // Shift remaining entries left
                                        for(int j = i; j < meta->access_count - 1; j++) {
                                            meta->access_list[j] = meta->access_list[j+1];
                                        }
                                        meta->access_count--;
                                        removed_count++;
                                        // Don't increment i, check same position again
                                    } else {
                                        i++;
                                    }
                                }
                                if (removed_count > 0) {
                                    log_event("  -> Access removed (%d entries).", removed_count);
                                    
                                    // Relay to SS for persistence
                                    int ss_sock = meta->ss_sock_fd;
                                    if (ss_state[ss_sock].active) {
                                        Header ss_header; ss_header.type = REQ_SS_REM_ACCESS; ss_header.payload_size = sizeof(Msg_Access_Request);
                                        send(ss_sock, &ss_header, sizeof(ss_header), 0);
                                        send(ss_sock, &req, sizeof(req), 0);
                                    }
                                }
                                send_ok_response(sock_fd);
                                break;
                            }
                            
                            // --- NEW FOR EXEC ---
                            case REQ_EXEC: {
                                Msg_Filename_Request req; recv(sock_fd, &req, sizeof(req), 0);
                                log_event("Got REQ_EXEC for '%s' from client '%s'", req.filename, client_state[sock_fd].username);
                                
                                int slot = find_file_slot(req.filename);
                                if (slot == -1) { log_event("  -> Error: File not found."); send_simple_header(sock_fd, RES_ERROR_NOT_FOUND); break; }
                                
                                FileMetadata* meta = &file_catalog[slot];
                                if (!has_read_access(meta, client_state[sock_fd].username)) {
                                    log_event("  -> Access Denied for user '%s'.", client_state[sock_fd].username);
                                    send_simple_header(sock_fd, RES_ERROR_ACCESS_DENIED); break;
                                }

                                SSInfo* ss = &ss_state[meta->ss_sock_fd];
                                if (!ss->active) { log_event("  -> Error: SS for file is offline."); send_simple_header(sock_fd, RES_ERROR); break; }
                                
                                // 1. Get file content from SS
                                log_event("  -> NM acting as client to fetch file content from SS...");
                                char* script_content = get_file_content_from_ss(ss, req.filename);
                                if (script_content == NULL) {
                                    log_event("  -> ERROR: Failed to fetch script content from SS.");
                                    send_simple_header(sock_fd, RES_ERROR);
                                    break;
                                }
                                
                                // 2. Execute content using popen()
                                // WARNING: This is a major security risk, as specified.
                                log_event("  -> Executing script: \n---\n%s\n---", script_content);
                                FILE* pipe = popen(script_content, "r");
                                free(script_content); // Free the buffer
                                
                                if (pipe == NULL) {
                                    log_event("  -> ERROR: popen() failed.");
                                    send_simple_header(sock_fd, RES_ERROR);
                                    break;
                                }

                                // 3. Stream output back to client
                                char line_buffer[FILE_BUFFER_SIZE];
                                while (fgets(line_buffer, sizeof(line_buffer), pipe) != NULL) {
                                    Header out_hdr;
                                    out_hdr.type = RES_EXEC_OUTPUT;
                                    out_hdr.payload_size = sizeof(Msg_Exec_Output);
                                    
                                    Msg_Exec_Output out_msg;
                                    strncpy(out_msg.line, line_buffer, FILE_BUFFER_SIZE);
                                    
                                    send(sock_fd, &out_hdr, sizeof(out_hdr), 0);
                                    send(sock_fd, &out_msg, sizeof(out_msg), 0);
                                }
                                
                                pclose(pipe);
                                
                                // 4. Send "DONE" packet
                                log_event("  -> Execution finished. Sending DONE.");
                                send_simple_header(sock_fd, RES_EXEC_DONE);
                                break;
                            }
                            // --- END EXEC ---

                            // --- FOLDER OPERATIONS ---
                            case REQ_CREATEFOLDER: {
                                Msg_Folder_Request req; recv(sock_fd, &req, sizeof(req), 0);
                                log_event("Got REQ_CREATEFOLDER for '%s' from client '%s'", req.foldername, client_state[sock_fd].username);
                                
                                // Find an available SS to store the folder
                                int ss_sock = find_available_ss();
                                if (ss_sock == -1) {
                                    log_event("  -> Error: No Storage Servers available.");
                                    send_simple_header(sock_fd, RES_ERROR);
                                    break;
                                }
                                
                                // Relay to SS
                                log_event("  -> Relaying REQ_SS_CREATEFOLDER to SS on socket %d", ss_sock);
                                Header ss_header; ss_header.type = REQ_SS_CREATEFOLDER; ss_header.payload_size = sizeof(Msg_Folder_Request);
                                send(ss_sock, &ss_header, sizeof(ss_header), 0);
                                send(ss_sock, &req, sizeof(req), 0);
                                
                                // Wait for SS response
                                recv(ss_sock, &header, sizeof(Header), 0);
                                if (header.type == RES_OK) {
                                    log_event("  -> Folder '%s' created successfully.", req.foldername);
                                    send_ok_response(sock_fd);
                                } else {
                                    log_event("  -> SS failed to create folder.");
                                    send_simple_header(sock_fd, RES_ERROR);
                                }
                                break;
                            }
                            
                            case REQ_MOVE: {
                                Msg_Move_Request req; recv(sock_fd, &req, sizeof(req), 0);
                                log_event("Got REQ_MOVE for '%s' to folder '%s' from client '%s'", req.filename, req.foldername, client_state[sock_fd].username);
                                
                                // Find the file
                                int slot = find_file_slot(req.filename);
                                if (slot == -1) {
                                    log_event("  -> Error: File not found.");
                                    send_simple_header(sock_fd, RES_ERROR_NOT_FOUND);
                                    break;
                                }
                                
                                FileMetadata* meta = &file_catalog[slot];
                                
                                // Check permissions (only owner can move)
                                if (strcmp(meta->owner, client_state[sock_fd].username) != 0) {
                                    log_event("  -> Access Denied. User '%s' is not owner.", client_state[sock_fd].username);
                                    send_simple_header(sock_fd, RES_ERROR_ACCESS_DENIED);
                                    break;
                                }
                                
                                int ss_sock = meta->ss_sock_fd;
                                if (!ss_state[ss_sock].active) {
                                    send_simple_header(sock_fd, RES_ERROR);
                                    break;
                                }
                                
                                // Relay to SS
                                log_event("  -> Relaying REQ_SS_MOVE to SS %d", ss_sock);
                                Header ss_header; ss_header.type = REQ_SS_MOVE; ss_header.payload_size = sizeof(Msg_Move_Request);
                                send(ss_sock, &ss_header, sizeof(ss_header), 0);
                                send(ss_sock, &req, sizeof(req), 0);
                                
                                // Wait for SS response
                                recv(ss_sock, &header, sizeof(Header), 0);
                                if (header.type == RES_OK) {
                                    // Extract just the filename (without old folder path)
                                    char* base_filename = strrchr(req.filename, '/');
                                    if (base_filename) {
                                        base_filename++; // Skip the '/'
                                    } else {
                                        base_filename = req.filename; // No folder, use as-is
                                    }
                                    
                                    // Update filename in catalog to include new folder path
                                    char new_path[MAX_FILENAME * 2];
                                    snprintf(new_path, sizeof(new_path), "%s/%s", req.foldername, base_filename);
                                    
                                    // Remove old hash/cache entry
                                    remove_from_hashmap(file_catalog[slot].filename);
                                    invalidate_cache(file_catalog[slot].filename);
                                    
                                    // Update filename
                                    strncpy(file_catalog[slot].filename, new_path, MAX_FILENAME);
                                    
                                    // Add new hash/cache entry
                                    add_to_hashmap(new_path, slot);
                                    
                                    log_event("  -> File moved successfully. New path: '%s'", new_path);
                                    send_ok_response(sock_fd);
                                } else {
                                    log_event("  -> SS failed to move file.");
                                    send_simple_header(sock_fd, RES_ERROR);
                                }
                                break;
                            }
                            
                            case REQ_VIEWFOLDER: {
                                Msg_Folder_Request req; recv(sock_fd, &req, sizeof(req), 0);
                                log_event("Got REQ_VIEWFOLDER for '%s' from client '%s'", req.foldername, client_state[sock_fd].username);
                                
                                // First, check if folder exists on any storage server
                                int folder_exists = 0;
                                for (int i = 0; i < MAX_CONNECTIONS; i++) {
                                    if (ss_state[i].active) {
                                        // Ask this SS if folder exists
                                        send_simple_header(i, REQ_SS_CHECKFOLDER);
                                        send(i, &req, sizeof(req), 0);
                                        
                                        Header check_res;
                                        recv(i, &check_res, sizeof(check_res), 0);
                                        if (check_res.type == RES_OK) {
                                            folder_exists = 1;
                                            break;
                                        }
                                    }
                                }
                                
                                if (!folder_exists) {
                                    log_event("  -> Folder '%s' does not exist", req.foldername);
                                    send_simple_header(sock_fd, RES_ERROR);
                                    break;
                                }
                                
                                // Count files in this folder
                                int count = 0;
                                char folder_prefix[MAX_FILENAME * 2];
                                snprintf(folder_prefix, sizeof(folder_prefix), "%s/", req.foldername);
                                
                                for(int i = 0; i < MAX_FILES_IN_SYSTEM; i++) {
                                    if (!file_catalog[i].active) continue;
                                    
                                    // Check if filename starts with folder_prefix
                                    if (strncmp(file_catalog[i].filename, folder_prefix, strlen(folder_prefix)) == 0) {
                                        // Check if user has read access
                                        if (has_read_access(&file_catalog[i], client_state[sock_fd].username)) {
                                            count++;
                                        }
                                    }
                                }
                                
                                // Send count
                                Header res_hdr; res_hdr.type = RES_VIEW_HDR; res_hdr.payload_size = sizeof(Msg_View_Hdr);
                                Msg_View_Hdr view_hdr; view_hdr.file_count = count;
                                send(sock_fd, &res_hdr, sizeof(res_hdr), 0);
                                send(sock_fd, &view_hdr, sizeof(view_hdr), 0);
                                
                                // Send file items
                                for(int i = 0; i < MAX_FILES_IN_SYSTEM; i++) {
                                    if (!file_catalog[i].active) continue;
                                    
                                    if (strncmp(file_catalog[i].filename, folder_prefix, strlen(folder_prefix)) == 0) {
                                        if (has_read_access(&file_catalog[i], client_state[sock_fd].username)) {
                                            res_hdr.type = RES_VIEW_ITEM_SHORT;
                                            res_hdr.payload_size = sizeof(Msg_View_Item_Short);
                                            Msg_View_Item_Short item;
                                            // Send just the filename without folder prefix
                                            strncpy(item.filename, file_catalog[i].filename + strlen(folder_prefix), MAX_FILENAME);
                                            send(sock_fd, &res_hdr, sizeof(res_hdr), 0);
                                            send(sock_fd, &item, sizeof(item), 0);
                                        }
                                    }
                                }
                                break;
                            }
                            // --- END FOLDER OPERATIONS ---

                            default:
                                log_event("Socket %d: Unknown message type %d", sock_fd, header.type);
                                break;
                        }
                    }
                }
            }
        }
    }
    return 0;
}