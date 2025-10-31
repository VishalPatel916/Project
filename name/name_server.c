#include "../protocol/protocol.h"
#include <sys/select.h>
#include <netinet/in.h>

// Helper to send a simple "OK" response
void send_ok_response(int sock) {
    Header header;
    header.type = RES_OK;
    header.payload_size = 0; // No data follows
    
    if (send(sock, &header, sizeof(Header), 0) < 0) {
        perror("Failed to send OK response");
    }
}

int main() {
    int listener_sock, new_sock, sock_fd;
    int fdmax; // Max socket number
    struct sockaddr_in nm_addr, client_addr;
    socklen_t client_len;

    // These `fd_set`s are the "switchboards" for select()
    fd_set master_set, read_set;

    // --- 1. Setup the Listener Socket ---
    listener_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listener_sock < 0) error_exit("socket");

    // Allow port reuse
    int yes = 1;
    if (setsockopt(listener_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) < 0) {
        error_exit("setsockopt");
    }

    // Bind to the public port
    memset(&nm_addr, 0, sizeof(nm_addr));
    nm_addr.sin_family = AF_INET;
    nm_addr.sin_addr.s_addr = INADDR_ANY; // Listen on all IPs
    nm_addr.sin_port = htons(NM_PORT);
    if (bind(listener_sock, (struct sockaddr *)&nm_addr, sizeof(nm_addr)) < 0) {
        error_exit("bind");
    }

    // Listen
    if (listen(listener_sock, 10) < 0) error_exit("listen");

    // --- 2. Setup `select()` ---
    FD_ZERO(&master_set); // Clear the master list
    FD_SET(listener_sock, &master_set); // Add the listener
    fdmax = listener_sock; // Keep track of the highest socket #

    printf("Name Server listening on port %d...\n", NM_PORT);

    // --- 3. Main Server Loop ---
    while (1) {
        read_set = master_set; // Copy master list to temp list
        
        // select() blocks (sleeps) until one of the sockets has activity
        if (select(fdmax + 1, &read_set, NULL, NULL, NULL) < 0) {
            error_exit("select");
        }

        // --- 4. Check for Activity ---
        for (sock_fd = 0; sock_fd <= fdmax; sock_fd++) {
            if (FD_ISSET(sock_fd, &read_set)) {
                
                // A) --- Activity is on the LISTENER: A New Connection! ---
                if (sock_fd == listener_sock) {
                    client_len = sizeof(client_addr);
                    new_sock = accept(listener_sock, (struct sockaddr *)&client_addr, &client_len);
                    
                    if (new_sock < 0) {
                        perror("accept");
                    } else {
                        FD_SET(new_sock, &master_set); // Add new socket to master list
                        if (new_sock > fdmax) fdmax = new_sock; // Update max
                        printf("New connection from %s on socket %d\n", inet_ntoa(client_addr.sin_addr), new_sock);
                    }
                }
                // B) --- Activity is on an EXISTING socket: A Message! ---
                else {
                    Header header;
                    int nbytes = recv(sock_fd, &header, sizeof(Header), 0);
                    
                    if (nbytes <= 0) {
                        // Client disconnected
                        printf("Socket %d disconnected.\n", sock_fd);
                        close(sock_fd);
                        FD_CLR(sock_fd, &master_set); // Remove from master list
                    } 
                    // C) --- Process the Message ---
                    else {
                        // We got a header! Find out what it is.
                        switch (header.type) {
                            case REQ_CLIENT_REGISTER: {
                                Msg_Client_Register msg;
                                recv(sock_fd, &msg, sizeof(msg), 0); // Read the payload
                                printf("Socket %d: CLIENT registered as '%s'\n", sock_fd, msg.username);
                                // TODO: Add user to your internal data structure
                                send_ok_response(sock_fd);
                                break;
                            }
                            case REQ_SS_REGISTER: {
                                Msg_SS_Register msg;
                                recv(sock_fd, &msg, sizeof(msg), 0); // Read the payload
                                printf("Socket %d: STORAGE SERVER registered at %s:%d\n", sock_fd, msg.ss_ip, msg.client_port);
                                // TODO: Add SS to your internal data structure
                                send_ok_response(sock_fd);
                                break;
                            }
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