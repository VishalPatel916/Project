#include "protocol.h"
#include <sys/select.h>
#include <netinet/in.h>

// --- 1. NM's Internal Data Structures (C-Style) ---

// Max connections is limited by FD_SETSIZE (usually 1024)
#define MAX_CONNECTIONS FD_SETSIZE

// Info for a connected client
typedef struct {
    int active; // 0 = inactive, 1 = active
    char username[MAX_USERNAME];
} ClientInfo;

// Info for a connected Storage Server
typedef struct {
    int active; // 0 = inactive, 1 = active
    char ip[MAX_IP_LEN];
    int client_port;
} SSInfo;

// Info for a single file in the catalog
typedef struct {
    int active; // 0 = inactive, 1 = active
    char filename[MAX_FILENAME];
    int ss_sock_fd; // Which SS has this file?
    // char owner[MAX_USERNAME]; // We will add this in Phase 2
} FileMetadata;

// --- 2. The NM's Global State ---
// We use static arrays, indexed by the socket descriptor (fd)
ClientInfo client_state[MAX_CONNECTIONS];
SSInfo ss_state[MAX_CONNECTIONS];
// For the file catalog, we use a simple array (O(N) search)
FileMetadata file_catalog[MAX_FILES_IN_SYSTEM];


// Helper to send a simple "OK" response
void send_ok_response(int sock) {
    Header header;
    header.type = RES_OK;
    header.payload_size = 0;
    if (send(sock, &header, sizeof(Header), 0) < 0) {
        perror("Failed to send OK response");
    }
}

// Helper to gracefully remove a disconnected socket
void handle_disconnect(int sock_fd) {
    printf("Socket %d disconnected.\n", sock_fd);

    // --- 1. Was it a client? ---
    if (client_state[sock_fd].active) {
        printf("Client '%s' disconnected.\n", client_state[sock_fd].username);
        client_state[sock_fd].active = 0;
    }
    // --- 2. Was it a storage server? ---
    else if (ss_state[sock_fd].active) {
        printf("Storage Server on socket %d disconnected.\n", sock_fd);
        ss_state[sock_fd].active = 0;
        
        // CRITICAL: Remove all files from catalog that this SS hosted
        printf("De-listing its files from the catalog...\n");
        int files_removed = 0;
        for (int i = 0; i < MAX_FILES_IN_SYSTEM; i++) {
            if (file_catalog[i].active && file_catalog[i].ss_sock_fd == sock_fd) {
                file_catalog[i].active = 0; // This slot is now free
                printf("  -> De-listed '%s'\n", file_catalog[i].filename);
                files_removed++;
            }
        }
        printf("Removed %d file entries.\n", files_removed);
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
    // Set all state slots to inactive
    printf("Initializing state...\n");
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        client_state[i].active = 0;
        ss_state[i].active = 0;
    }
    for (int i = 0; i < MAX_FILES_IN_SYSTEM; i++) {
        file_catalog[i].active = 0;
    }

    // --- 4. Setup the Listener Socket ---
    listener_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listener_sock < 0) error_exit("socket");

    int yes = 1;
    if (setsockopt(listener_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) < 0) {
        error_exit("setsockopt");
    }

    memset(&nm_addr, 0, sizeof(nm_addr));
    nm_addr.sin_family = AF_INET;
    nm_addr.sin_addr.s_addr = INADDR_ANY;
    nm_addr.sin_port = htons(NM_PORT);
    if (bind(listener_sock, (struct sockaddr *)&nm_addr, sizeof(nm_addr)) < 0) {
        error_exit("bind");
    }

    if (listen(listener_sock, 10) < 0) error_exit("listen");

    // --- 5. Setup `select()` ---
    FD_ZERO(&master_set);
    FD_SET(listener_sock, &master_set);
    fdmax = listener_sock;

    printf("Name Server listening on port %d...\n", NM_PORT);

    // --- 6. Main Server Loop ---
    while (1) {
        read_set = master_set;
        
        if (select(fdmax + 1, &read_set, NULL, NULL, NULL) < 0) {
            error_exit("select");
        }

        // --- 7. Check for Activity ---
        for (sock_fd = 0; sock_fd <= fdmax; sock_fd++) {
            if (FD_ISSET(sock_fd, &read_set)) {
                
                // A) --- New Connection! ---
                if (sock_fd == listener_sock) {
                    new_sock = accept(listener_sock, (struct sockaddr *)&client_addr, &client_len);
                    if (new_sock < 0) {
                        perror("accept");
                    } else {
                        FD_SET(new_sock, &master_set);
                        if (new_sock > fdmax) fdmax = new_sock;
                        printf("New connection from %s on socket %d\n", inet_ntoa(client_addr.sin_addr), new_sock);
                    }
                }
                // B) --- Existing Socket Activity ---
                else {
                    Header header;
                    int nbytes = recv(sock_fd, &header, sizeof(Header), 0);
                    
                    if (nbytes <= 0) {
                        // C) --- Client/SS Disconnected ---
                        handle_disconnect(sock_fd); // Use our new helper
                        FD_CLR(sock_fd, &master_set);
                    } 
                    // D) --- Process the Message ---
                    else {
                        switch (header.type) {
                            case REQ_CLIENT_REGISTER: {
                                Msg_Client_Register msg;
                                recv(sock_fd, &msg, sizeof(msg), 0);
                                printf("Socket %d: CLIENT registered as '%s'\n", sock_fd, msg.username);
                                
                                // --- ADD TO STATE ---
                                client_state[sock_fd].active = 1;
                                strncpy(client_state[sock_fd].username, msg.username, MAX_USERNAME);
                                ss_state[sock_fd].active = 0; // Make sure it's not marked as an SS
                                
                                send_ok_response(sock_fd);
                                break;
                            }
                            case REQ_SS_REGISTER: {
                                Msg_SS_Register msg;
                                recv(sock_fd, &msg, sizeof(msg), 0);
                                printf("Socket %d: STORAGE SERVER registered at %s:%d. Expecting %d files.\n", 
                                       sock_fd, msg.ss_ip, msg.client_port, msg.file_count);
                                
                                // --- ADD TO STATE ---
                                ss_state[sock_fd].active = 1;
                                strncpy(ss_state[sock_fd].ip, msg.ss_ip, MAX_IP_LEN);
                                ss_state[sock_fd].client_port = msg.client_port;
                                client_state[sock_fd].active = 0; // Not a client

                                // --- POPULATE FILE CATALOG ---
                                printf("Receiving file list...\n");
                                for (int i = 0; i < msg.file_count; i++) {
                                    Msg_File_Item item;
                                    if (recv(sock_fd, &item, sizeof(item), 0) <= 0) {
                                        perror("SS disconnected during file list");
                                        break; 
                                    }
                                    
                                    // Find an empty slot in the catalog
                                    int slot = -1;
                                    for (int j = 0; j < MAX_FILES_IN_SYSTEM; j++) {
                                        if (file_catalog[j].active == 0) {
                                            slot = j;
                                            break;
                                        }
                                    }
                                    
                                    if (slot != -1) {
                                        file_catalog[slot].active = 1;
                                        strncpy(file_catalog[slot].filename, item.filename, MAX_FILENAME);
                                        file_catalog[slot].ss_sock_fd = sock_fd;
                                        printf("  -> Cataloged '%s' (from SS %d) in slot %d\n", item.filename, sock_fd, slot);
                                    } else {
                                        printf("  -> ERROR: File catalog is full! Could not add '%s'\n", item.filename);
                                    }
                                }
                                
                                send_ok_response(sock_fd);
                                break;
                            }
                            default:
                                printf("Socket %d: Unknown message type %d\n", sock_fd, header.type);
                                // Try to clear the socket buffer (basic)
                                if (header.payload_size > 0) {
                                    char junk_buffer[1024];
                                    int to_read = header.payload_size;
                                    while(to_read > 0) {
                                        int read_now = (to_read > 1024) ? 1024 : to_read;
                                        recv(sock_fd, junk_buffer, read_now, 0);
                                        to_read -= read_now;
                                    }
                                }
                                break;
                        }
                    }
                }
            }
        }
    }
    return 0;
}