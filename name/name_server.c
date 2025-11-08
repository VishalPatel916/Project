#include "protocol.h"
#include <sys/select.h>
#include <netinet/in.h>
#include <time.h>     // For logging
#include <stdarg.h>   // For logging

// --- NEW: Global Log File ---
FILE* nm_log_file;

// --- 1. NM's Internal Data Structures (C-Style) ---
#define MAX_CONNECTIONS FD_SETSIZE
typedef struct {
    int active;
    char username[MAX_USERNAME];
    char ip_addr[MAX_IP_LEN]; // <-- NEW
} ClientInfo;

typedef struct {
    int active;
    char ip[MAX_IP_LEN];
    int client_port;
    char ip_addr[MAX_IP_LEN]; // <-- NEW
} SSInfo;

typedef struct {
    int active;
    char filename[MAX_FILENAME];
    int ss_sock_fd; 
    char owner[MAX_USERNAME];
} FileMetadata;

// --- 2. The NM's Global State ---
ClientInfo client_state[MAX_CONNECTIONS];
SSInfo ss_state[MAX_CONNECTIONS];
FileMetadata file_catalog[MAX_FILES_IN_SYSTEM];


// --- NEW: Logging Function ---
void log_event(const char* format, ...) {
    char time_buf[50];
    time_t now = time(NULL);
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", localtime(&now));
    
    va_list args;
    
    // Print to stdout
    printf("[%s] ", time_buf);
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");

    // Print to log file
    fprintf(nm_log_file, "[%s] ", time_buf);
    va_start(args, format);
    vfprintf(nm_log_file, format, args);
    va_end(args);
    fprintf(nm_log_file, "\n");
    
    fflush(nm_log_file); // Ensure it's written immediately
}


// --- 3. Helper Functions ---
int find_file_slot(char* filename) { for (int i = 0; i < MAX_FILES_IN_SYSTEM; i++) { if (file_catalog[i].active && strcmp(file_catalog[i].filename, filename) == 0) return i; } return -1; }
int find_empty_file_slot() { for (int i = 0; i < MAX_FILES_IN_SYSTEM; i++) { if (file_catalog[i].active == 0) return i; } return -1; }
int find_available_ss() { for (int i = 0; i < MAX_CONNECTIONS; i++) { if (ss_state[i].active) return i; } return -1; }
void send_ok_response(int sock) { Header header; header.type = RES_OK; header.payload_size = 0; if (send(sock, &header, sizeof(Header), 0) < 0) { log_event("Failed to send OK response to socket %d", sock); } }

// --- Updated handle_disconnect ---
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
                file_catalog[i].active = 0;
                log_event("  -> De-listed '%s'", file_catalog[i].filename);
            }
        }
    } else {
        log_event("Socket %d (unregistered) disconnected.", sock_fd);
    }
    close(sock_fd);
}


int main() {
    int listener_sock, new_sock, sock_fd;
    int fdmax;
    struct sockaddr_in nm_addr, client_addr;
    socklen_t client_len;
    fd_set master_set, read_set;

    // --- Open Log File ---
    nm_log_file = fopen("nm.log", "a");
    if (nm_log_file == NULL) error_exit("fopen nm.log");
    
    // --- 3. Initialize State ---
    log_event("--- Name Server Started ---");
    log_event("Initializing state...");
    for (int i = 0; i < MAX_CONNECTIONS; i++) { client_state[i].active = 0; ss_state[i].active = 0; }
    for (int i = 0; i < MAX_FILES_IN_SYSTEM; i++) { file_catalog[i].active = 0; }

    // --- 4. Setup the Listener Socket ---
    listener_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listener_sock < 0) error_exit("socket");
    int yes = 1;
    if (setsockopt(listener_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) < 0) error_exit("setsockopt");
    memset(&nm_addr, 0, sizeof(nm_addr));
    nm_addr.sin_family = AF_INET;
    nm_addr.sin_addr.s_addr = INADDR_ANY;
    nm_addr.sin_port = htons(NM_PORT);
    if (bind(listener_sock, (struct sockaddr *)&nm_addr, sizeof(nm_addr)) < 0) error_exit("bind");
    if (listen(listener_sock, 10) < 0) error_exit("listen");

    // --- 5. Setup `select()` ---
    FD_ZERO(&master_set);
    FD_SET(listener_sock, &master_set);
    fdmax = listener_sock;
    log_event("Name Server listening on port %d...", NM_PORT);

    // --- 6. Main Server Loop ---
    while (1) {
        read_set = master_set;
        if (select(fdmax + 1, &read_set, NULL, NULL, NULL) < 0) { log_event("select() error"); error_exit("select"); }

        // --- 7. Check for Activity ---
        for (sock_fd = 0; sock_fd <= fdmax; sock_fd++) {
            if (FD_ISSET(sock_fd, &read_set)) {
                
                if (sock_fd == listener_sock) { // A) New Connection
                    client_len = sizeof(client_addr);
                    new_sock = accept(listener_sock, (struct sockaddr *)&client_addr, &client_len);
                    if (new_sock < 0) { perror("accept"); }
                    else {
                        FD_SET(new_sock, &master_set);
                        if (new_sock > fdmax) fdmax = new_sock;
                        log_event("New connection from %s on socket %d", inet_ntoa(client_addr.sin_addr), new_sock);
                    }
                } 
                else { // B) Existing Socket
                    Header header;
                    int nbytes = recv(sock_fd, &header, sizeof(Header), 0);
                    if (nbytes <= 0) { // C) Disconnect
                        handle_disconnect(sock_fd);
                        FD_CLR(sock_fd, &master_set);
                    } 
                    else { // D) Process Message
                        // Get IP for logging (we'll only use this if they aren't registered yet)
                        struct sockaddr_in addr; socklen_t len = sizeof(addr);
                        getpeername(sock_fd, (struct sockaddr*)&addr, &len);
                        char* peer_ip = inet_ntoa(addr.sin_addr);

                        switch (header.type) {
                            case REQ_CLIENT_REGISTER: {
                                Msg_Client_Register msg; recv(sock_fd, &msg, sizeof(msg), 0);
                                log_event("Socket %d (%s): CLIENT registered as '%s'", sock_fd, peer_ip, msg.username);
                                client_state[sock_fd].active = 1; strncpy(client_state[sock_fd].username, msg.username, MAX_USERNAME);
                                strncpy(client_state[sock_fd].ip_addr, peer_ip, MAX_IP_LEN); // Store IP
                                ss_state[sock_fd].active = 0;
                                send_ok_response(sock_fd);
                                break;
                            }
                            case REQ_SS_REGISTER: {
                                Msg_SS_Register msg; recv(sock_fd, &msg, sizeof(msg), 0);
                                log_event("Socket %d (%s): SS registered at %s:%d. Expecting %d files.", sock_fd, peer_ip, msg.ss_ip, msg.client_port, msg.file_count);
                                ss_state[sock_fd].active = 1; strncpy(ss_state[sock_fd].ip, msg.ss_ip, MAX_IP_LEN);
                                ss_state[sock_fd].client_port = msg.client_port;
                                strncpy(ss_state[sock_fd].ip_addr, peer_ip, MAX_IP_LEN); // Store IP
                                client_state[sock_fd].active = 0;
                                log_event("Receiving file list from SS %d...", sock_fd);
                                for (int i = 0; i < msg.file_count; i++) {
                                    Msg_File_Item item; recv(sock_fd, &item, sizeof(item), 0);
                                    int slot = find_empty_file_slot();
                                    if (slot != -1) {
                                        file_catalog[slot].active = 1; strncpy(file_catalog[slot].filename, item.filename, MAX_FILENAME);
                                        file_catalog[slot].ss_sock_fd = sock_fd; strncpy(file_catalog[slot].owner, "system", MAX_USERNAME); 
                                        log_event("  -> Cataloged '%s' (owner: system) in slot %d", item.filename, slot);
                                    }
                                }
                                send_ok_response(sock_fd);
                                break;
                            }
                            case REQ_CREATE: {
                                Msg_Filename_Request req; recv(sock_fd, &req, sizeof(req), 0);
                                log_event("Got REQ_CREATE for '%s' from client '%s' (Sock %d)", req.filename, client_state[sock_fd].username, sock_fd);
                                if (find_file_slot(req.filename) != -1) {
                                    log_event("  -> Error: File '%s' already exists.", req.filename);
                                    send_simple_header(sock_fd, RES_ERROR_FILE_EXISTS);
                                } else {
                                    int ss_sock = find_available_ss();
                                    if (ss_sock == -1) { log_event("  -> Error: No Storage Servers available."); send_simple_header(sock_fd, RES_ERROR); }
                                    else {
                                        log_event("  -> Relaying REQ_SS_CREATE to SS on socket %d", ss_sock);
                                        Header ss_header; ss_header.type = REQ_SS_CREATE; ss_header.payload_size = sizeof(Msg_Filename_Request);
                                        send(ss_sock, &ss_header, sizeof(ss_header), 0); send(ss_sock, &req, sizeof(req), 0);
                                        recv(ss_sock, &header, sizeof(Header), 0); 
                                        if (header.type == RES_OK) {
                                            log_event("  -> SS confirmed creation. Updating catalog.");
                                            int slot = find_empty_file_slot();
                                            file_catalog[slot].active = 1; strncpy(file_catalog[slot].filename, req.filename, MAX_FILENAME);
                                            file_catalog[slot].ss_sock_fd = ss_sock; strncpy(file_catalog[slot].owner, client_state[sock_fd].username, MAX_USERNAME);
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
                                if(header.type == REQ_READ) log_event("Got REQ_READ for '%s' from client '%s' (Sock %d)", req.filename, client_state[sock_fd].username, sock_fd);
                                else if(header.type == REQ_WRITE) log_event("Got REQ_WRITE for '%s' from client '%s' (Sock %d)", req.filename, client_state[sock_fd].username, sock_fd);
                                else log_event("Got REQ_STREAM for '%s' from client '%s' (Sock %d)", req.filename, client_state[sock_fd].username, sock_fd);

                                int slot = find_file_slot(req.filename);
                                if (slot == -1) {
                                    log_event("  -> Error: File '%s' not found.", req.filename);
                                    send_simple_header(sock_fd, RES_ERROR_NOT_FOUND);
                                } else {
                                    FileMetadata* meta = &file_catalog[slot];
                                    SSInfo* ss = &ss_state[meta->ss_sock_fd];
                                    if (!ss->active) { log_event("  -> Error: SS for file is offline."); send_simple_header(sock_fd, RES_ERROR); }
                                    else {
                                        log_event("  -> File found on SS %d (%s:%d)", meta->ss_sock_fd, ss->ip, ss->client_port);
                                        Header res_hdr; res_hdr.type = RES_READ_LOCATION; res_hdr.payload_size = sizeof(Msg_Read_Response);
                                        Msg_Read_Response res_payload; strncpy(res_payload.ss_ip, ss->ip, MAX_IP_LEN); res_payload.ss_port = ss->client_port;
                                        send(sock_fd, &res_hdr, sizeof(res_hdr), 0);
                                        send(sock_fd, &res_payload, sizeof(res_payload), 0);
                                    }
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
                                log_event("Got REQ_DELETE for '%s' from client '%s' (Sock %d)", req.filename, client_state[sock_fd].username, sock_fd);
                                int slot = find_file_slot(req.filename);
                                if (slot == -1) { send_simple_header(sock_fd, RES_ERROR_NOT_FOUND); break; }
                                FileMetadata* meta = &file_catalog[slot];
                                if (strcmp(meta->owner, client_state[sock_fd].username) != 0) {
                                    log_event("  -> Access Denied. User '%s' is not owner '%s'.", client_state[sock_fd].username, meta->owner);
                                    send_simple_header(sock_fd, RES_ERROR_ACCESS_DENIED);
                                    break;
                                }
                                int ss_sock = meta->ss_sock_fd;
                                if (!ss_state[ss_sock].active) { send_simple_header(sock_fd, RES_ERROR); break; }
                                log_event("  -> Relaying REQ_SS_DELETE to SS %d", ss_sock);
                                Header ss_header; ss_header.type = REQ_SS_DELETE; ss_header.payload_size = sizeof(Msg_Filename_Request);
                                send(ss_sock, &ss_header, sizeof(ss_header), 0); send(ss_sock, &req, sizeof(req), 0);
                                recv(ss_sock, &header, sizeof(Header), 0);
                                file_catalog[slot].active = 0;
                                send_ok_response(sock_fd);
                                break;
                            }
                            case REQ_UNDO: {
                                Msg_Filename_Request req; recv(sock_fd, &req, sizeof(req), 0);
                                log_event("Got REQ_UNDO for '%s' from client '%s' (Sock %d)", req.filename, client_state[sock_fd].username, sock_fd);
                                int slot = find_file_slot(req.filename);
                                if (slot == -1) { send_simple_header(sock_fd, RES_ERROR_NOT_FOUND); break; }
                                FileMetadata* meta = &file_catalog[slot];
                                if (strcmp(meta->owner, client_state[sock_fd].username) != 0) {
                                    log_event("  -> Access Denied (UNDO). User '%s' is not owner '%s'.", client_state[sock_fd].username, meta->owner);
                                    send_simple_header(sock_fd, RES_ERROR_ACCESS_DENIED);
                                    break;
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