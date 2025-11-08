#include "protocol.h"
#include <sys/select.h>
#include <netinet/in.h>

// --- 1. NM's Internal Data Structures (C-Style) ---
#define MAX_CONNECTIONS FD_SETSIZE
typedef struct { int active; char username[MAX_USERNAME]; } ClientInfo;
typedef struct { int active; char ip[MAX_IP_LEN]; int client_port; } SSInfo;
typedef struct {
    int active;
    char filename[MAX_FILENAME];
    int ss_sock_fd; // Which SS has this file?
    char owner[MAX_USERNAME]; // NEW: We now track the owner
} FileMetadata;

// --- 2. The NM's Global State ---
ClientInfo client_state[MAX_CONNECTIONS];
SSInfo ss_state[MAX_CONNECTIONS];
FileMetadata file_catalog[MAX_FILES_IN_SYSTEM];


// --- Helper: Find a file slot by name ---
int find_file_slot(char* filename) {
    for (int i = 0; i < MAX_FILES_IN_SYSTEM; i++) {
        if (file_catalog[i].active && strcmp(file_catalog[i].filename, filename) == 0) {
            return i; // Found it
        }
    }
    return -1; // Not found
}

// --- Helper: Find an empty file slot ---
int find_empty_file_slot() {
    for (int i = 0; i < MAX_FILES_IN_SYSTEM; i++) {
        if (file_catalog[i].active == 0) {
            return i;
        }
    }
    return -1; // Catalog is full
}

// --- Helper: Find first available SS ---
int find_available_ss() {
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (ss_state[i].active) {
            return i; // Return the socket fd of the first active SS
        }
    }
    return -1; // No SS available
}

// --- Helper: Send a simple "OK" response ---
void send_ok_response(int sock) {
    Header header; header.type = RES_OK; header.payload_size = 0;
    if (send(sock, &header, sizeof(Header), 0) < 0) { perror("Failed to send OK response"); }
}

// --- Helper: Gracefully remove a disconnected socket ---
void handle_disconnect(int sock_fd) {
    printf("Socket %d disconnected.\n", sock_fd);
    if (client_state[sock_fd].active) {
        printf("Client '%s' disconnected.\n", client_state[sock_fd].username);
        client_state[sock_fd].active = 0;
    } else if (ss_state[sock_fd].active) {
        printf("Storage Server on socket %d disconnected.\n", sock_fd);
        ss_state[sock_fd].active = 0;
        printf("De-listing its files from the catalog...\n");
        for (int i = 0; i < MAX_FILES_IN_SYSTEM; i++) {
            if (file_catalog[i].active && file_catalog[i].ss_sock_fd == sock_fd) {
                file_catalog[i].active = 0;
                printf("  -> De-listed '%s'\n", file_catalog[i].filename);
            }
        }
    }
    close(sock_fd);
}


int main() {
    int listener_sock, new_sock, sock_fd;
    int fdmax;
    struct sockaddr_in nm_addr, client_addr;
    socklen_t client_len;
    fd_set master_set, read_set;

    // --- 3. Initialize State ---
    printf("Initializing state...\n");
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
    printf("Name Server listening on port %d...\n", NM_PORT);

    // --- 6. Main Server Loop ---
    while (1) {
        read_set = master_set;
        if (select(fdmax + 1, &read_set, NULL, NULL, NULL) < 0) error_exit("select");

        // --- 7. Check for Activity ---
        for (sock_fd = 0; sock_fd <= fdmax; sock_fd++) {
            if (FD_ISSET(sock_fd, &read_set)) {
                
                if (sock_fd == listener_sock) { // A) New Connection
                    new_sock = accept(listener_sock, (struct sockaddr *)&client_addr, &client_len);
                    if (new_sock < 0) { perror("accept"); }
                    else {
                        FD_SET(new_sock, &master_set);
                        if (new_sock > fdmax) fdmax = new_sock;
                        printf("New connection from %s on socket %d\n", inet_ntoa(client_addr.sin_addr), new_sock);
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
                        switch (header.type) {
                            case REQ_CLIENT_REGISTER: { // (Phase 1)
                                Msg_Client_Register msg;
                                recv(sock_fd, &msg, sizeof(msg), 0);
                                printf("Socket %d: CLIENT registered as '%s'\n", sock_fd, msg.username);
                                client_state[sock_fd].active = 1;
                                strncpy(client_state[sock_fd].username, msg.username, MAX_USERNAME);
                                ss_state[sock_fd].active = 0;
                                send_ok_response(sock_fd);
                                break;
                            }
                            case REQ_SS_REGISTER: { // (Phase 1)
                                Msg_SS_Register msg;
                                recv(sock_fd, &msg, sizeof(msg), 0);
                                printf("Socket %d: SS registered at %s:%d. Expecting %d files.\n", sock_fd, msg.ss_ip, msg.client_port, msg.file_count);
                                ss_state[sock_fd].active = 1;
                                strncpy(ss_state[sock_fd].ip, msg.ss_ip, MAX_IP_LEN);
                                ss_state[sock_fd].client_port = msg.client_port;
                                client_state[sock_fd].active = 0;
                                for (int i = 0; i < msg.file_count; i++) {
                                    Msg_File_Item item;
                                    recv(sock_fd, &item, sizeof(item), 0);
                                    int slot = find_empty_file_slot();
                                    if (slot != -1) {
                                        file_catalog[slot].active = 1;
                                        strncpy(file_catalog[slot].filename, item.filename, MAX_FILENAME);
                                        file_catalog[slot].ss_sock_fd = sock_fd;
                                        strncpy(file_catalog[slot].owner, "system", MAX_USERNAME); // Mark system owner
                                        printf("  -> Cataloged '%s' (owner: system) in slot %d\n", item.filename, slot);
                                    }
                                }
                                send_ok_response(sock_fd);
                                break;
                            }
                            // --- NEW FOR PHASE 2 ---
                            case REQ_CREATE: {
                                Msg_Filename_Request req;
                                recv(sock_fd, &req, sizeof(req), 0);
                                printf("Got REQ_CREATE for '%s' from client %d\n", req.filename, sock_fd);
                                
                                if (find_file_slot(req.filename) != -1) {
                                    // File already exists
                                    printf("  -> Error: File '%s' already exists.\n", req.filename);
                                    send_simple_header(sock_fd, RES_ERROR_FILE_EXISTS);
                                } else {
                                    // Find an SS to send it to
                                    int ss_sock = find_available_ss();
                                    if (ss_sock == -1) {
                                        printf("  -> Error: No Storage Servers available.\n");
                                        send_simple_header(sock_fd, RES_ERROR);
                                    } else {
                                        // Send REQ_SS_CREATE to the SS
                                        printf("  -> Relaying REQ_SS_CREATE to SS on socket %d\n", ss_sock);
                                        Header ss_header;
                                        ss_header.type = REQ_SS_CREATE;
                                        ss_header.payload_size = sizeof(Msg_Filename_Request);
                                        send(ss_sock, &ss_header, sizeof(ss_header), 0);
                                        send(ss_sock, &req, sizeof(req), 0);
                                        
                                        // Wait for OK from SS (This is blocking, see note in prev answer)
                                        recv(ss_sock, &header, sizeof(Header), 0); 
                                        
                                        if (header.type == RES_OK) {
                                            printf("  -> SS confirmed creation. Updating catalog.\n");
                                            // Add to catalog
                                            int slot = find_empty_file_slot();
                                            file_catalog[slot].active = 1;
                                            strncpy(file_catalog[slot].filename, req.filename, MAX_FILENAME);
                                            file_catalog[slot].ss_sock_fd = ss_sock;
                                            strncpy(file_catalog[slot].owner, client_state[sock_fd].username, MAX_USERNAME);
                                            
                                            send_ok_response(sock_fd); // Send OK to client
                                        } else {
                                            printf("  -> SS failed to create file.\n");
                                            send_simple_header(sock_fd, RES_ERROR);
                                        }
                                    }
                                }
                                break;
                            }
                            case REQ_READ: {
                                Msg_Filename_Request req;
                                recv(sock_fd, &req, sizeof(req), 0);
                                printf("Got REQ_READ for '%s' from client %d\n", req.filename, sock_fd);

                                int slot = find_file_slot(req.filename);
                                if (slot == -1) {
                                    // File not found
                                    printf("  -> Error: File '%s' not found.\n", req.filename);
                                    send_simple_header(sock_fd, RES_ERROR_NOT_FOUND);
                                } else {
                                    // File found! Get SS info
                                    FileMetadata* meta = &file_catalog[slot];
                                    SSInfo* ss = &ss_state[meta->ss_sock_fd];
                                    
                                    if (!ss->active) {
                                        printf("  -> Error: SS for file is offline.\n");
                                        send_simple_header(sock_fd, RES_ERROR);
                                    } else {
                                        // Send location back to client
                                        printf("  -> File found on SS %d (%s:%d)\n", meta->ss_sock_fd, ss->ip, ss->client_port);
                                        Header res_hdr;
                                        res_hdr.type = RES_READ_LOCATION;
                                        res_hdr.payload_size = sizeof(Msg_Read_Response);
                                        
                                        Msg_Read_Response res_payload;
                                        strncpy(res_payload.ss_ip, ss->ip, MAX_IP_LEN);
                                        res_payload.ss_port = ss->client_port;

                                        send(sock_fd, &res_hdr, sizeof(res_hdr), 0);
                                        send(sock_fd, &res_payload, sizeof(res_payload), 0);
                                    }
                                }
                                break;
                            }
                            // --- END NEW ---
                            default:
                                printf("Socket %d: Unknown message type %d\n", sock_fd, header.type);
                                break;
                        }
                    }
                }
            }
        }
    }
    return 0;
}