#include "protocol.h"
#include <netinet/in.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/select.h>
#include <fcntl.h> // For open()

#define MY_IP_FOR_CLIENTS "127.0.0.1"
#define MY_PORT_FOR_CLIENTS 8082
#define MY_STORAGE_PATH "./ss_storage"

// --- Helper: Create an empty file ---
void handle_create_file(char* filename) {
    char file_path[MAX_FILENAME + 100];
    sprintf(file_path, "%s/%s", MY_STORAGE_PATH, filename);

    printf("  -> Creating file at: %s\n", file_path);
    FILE* f = fopen(file_path, "w"); // "w" creates an empty file
    if (f) {
        fclose(f);
    } else {
        perror("fopen create file");
    }
}

// --- Helper: Send a file to a client (with handshake) ---
void handle_send_file(int client_sock, char* filename) {
    char file_path[MAX_FILENAME + 100];
    sprintf(file_path, "%s/%s", MY_STORAGE_PATH, filename);
    
    int fd = open(file_path, O_RDONLY);
    if (fd < 0) {
        perror("open read file");
        
        // --- TODO is DONE ---
        // Send a "file not found" error back to the client
        printf("  -> File not found. Sending error to socket %d\n", client_sock);
        send_simple_header(client_sock, RES_ERROR_NOT_FOUND);
        // --- End ---
        
        return;
    }

    // --- File was found! ---
    // 1. Send the "OK, file is coming" handshake message
    send_simple_header(client_sock, RES_SS_FILE_OK);

    // 2. Send the file data
    printf("  -> Sending file '%s' to socket %d\n", filename, client_sock);
    char buffer[FILE_BUFFER_SIZE];
    int bytes_read;
    while ((bytes_read = read(fd, buffer, FILE_BUFFER_SIZE)) > 0) {
        if (send(client_sock, buffer, bytes_read, 0) < 0) {
            perror("send file chunk");
            break; // Client disconnected
        }
    }
    
    close(fd);
    printf("  -> Finished sending file to socket %d\n", client_sock);
}


int main() {
    int nm_sock; // Socket for talking to Name Server
    int client_listener_sock, new_client_sock; // Sockets for talking to Clients
    struct sockaddr_in nm_addr, client_listen_addr, client_addr;
    socklen_t client_len;

    fd_set master_set, read_set;
    int fdmax;

    // --- 1. Scan storage directory (from Phase 1) ---
    char my_files[MAX_FILES_PER_SS][MAX_FILENAME];
    int file_count = 0;
    printf("Scanning storage directory: %s\n", MY_STORAGE_PATH);
    mkdir(MY_STORAGE_PATH, 0777); // Create directory if it doesn't exist
    
    DIR *d = opendir(MY_STORAGE_PATH);
    if (d) {
        struct dirent *dir;
        while ((dir = readdir(d)) != NULL && file_count < MAX_FILES_PER_SS) {
            if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0) continue;
            strncpy(my_files[file_count], dir->d_name, MAX_FILENAME);
            file_count++;
        }
        closedir(d);
    }
    printf("Found %d files.\n", file_count);

    // --- 2. Connect to Name Server (from Phase 1) ---
    nm_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (nm_sock < 0) error_exit("socket");
    memset(&nm_addr, 0, sizeof(nm_addr));
    nm_addr.sin_family = AF_INET;
    nm_addr.sin_port = htons(NM_PORT);
    if (inet_pton(AF_INET, "127.0.0.1", &nm_addr.sin_addr) <= 0) error_exit("inet_pton");
    if (connect(nm_sock, (struct sockaddr *)&nm_addr, sizeof(nm_addr)) < 0) error_exit("connect");
    printf("Storage Server connected to Name Server...\n");

    // --- 3. Register with Name Server (from Phase 1) ---
    Header header;
    header.type = REQ_SS_REGISTER;
    header.payload_size = sizeof(Msg_SS_Register);
    Msg_SS_Register reg_msg;
    strncpy(reg_msg.ss_ip, MY_IP_FOR_CLIENTS, MAX_IP_LEN);
    reg_msg.client_port = MY_PORT_FOR_CLIENTS;
    reg_msg.file_count = file_count;
    if (send(nm_sock, &header, sizeof(Header), 0) < 0) error_exit("send header");
    if (send(nm_sock, &reg_msg, sizeof(reg_msg), 0) < 0) error_exit("send payload");

    for (int i = 0; i < file_count; i++) {
        Msg_File_Item item;
        strncpy(item.filename, my_files[i], MAX_FILENAME);
        if (send(nm_sock, &item, sizeof(item), 0) < 0) error_exit("send file item");
    }
    
    if (recv(nm_sock, &header, sizeof(Header), 0) < 0 || header.type != RES_OK) {
        error_exit("Registration failed");
    }
    printf("Registration successful!\n");

    // --- 4. Setup Client Listener Socket (NEW) ---
    client_listener_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (client_listener_sock < 0) error_exit("client listener socket");
    int yes = 1;
    if (setsockopt(client_listener_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) < 0) {
        error_exit("setsockopt");
    }
    memset(&client_listen_addr, 0, sizeof(client_listen_addr));
    client_listen_addr.sin_family = AF_INET;
    client_listen_addr.sin_addr.s_addr = INADDR_ANY;
    client_listen_addr.sin_port = htons(MY_PORT_FOR_CLIENTS);
    if (bind(client_listener_sock, (struct sockaddr *)&client_listen_addr, sizeof(client_listen_addr)) < 0) {
        error_exit("client listener bind");
    }
    if (listen(client_listener_sock, 10) < 0) error_exit("client listener listen");
    printf("Storage Server now listening for clients on port %d...\n", MY_PORT_FOR_CLIENTS);


    // --- 5. Main Server Loop (NEW) ---
    FD_ZERO(&master_set);
    FD_SET(nm_sock, &master_set);
    FD_SET(client_listener_sock, &master_set);
    fdmax = (nm_sock > client_listener_sock) ? nm_sock : client_listener_sock;

    while (1) {
        read_set = master_set;
        if (select(fdmax + 1, &read_set, NULL, NULL, NULL) < 0) {
            error_exit("select");
        }

        for (int sock_fd = 0; sock_fd <= fdmax; sock_fd++) {
            if (FD_ISSET(sock_fd, &read_set)) {
                
                // A) --- Activity from Name Server ---
                if (sock_fd == nm_sock) {
                    if (recv(nm_sock, &header, sizeof(Header), 0) <= 0) {
                        error_exit("Name Server disconnected!");
                    }
                    switch (header.type) {
                        case REQ_SS_CREATE: {
                            Msg_Filename_Request req;
                            recv(nm_sock, &req, sizeof(req), 0);
                            printf("Got REQ_SS_CREATE for '%s' from NM\n", req.filename);
                            handle_create_file(req.filename);
                            send_simple_header(nm_sock, RES_OK); // Send OK back to NM
                            break;
                        }
                        default:
                            printf("Got unknown command %d from NM\n", header.type);
                    }
                }
                
                // B) --- New Client Connection ---
                else if (sock_fd == client_listener_sock) {
                    client_len = sizeof(client_addr);
                    new_client_sock = accept(client_listener_sock, (struct sockaddr *)&client_addr, &client_len);
                    if (new_client_sock < 0) {
                        perror("accept new client");
                    } else {
                        FD_SET(new_client_sock, &master_set);
                        if (new_client_sock > fdmax) fdmax = new_client_sock;
                        printf("New client connection on socket %d\n", new_client_sock);
                    }
                }
                
                // C) --- Activity from Existing Client ---
                else {
                    if (recv(sock_fd, &header, sizeof(Header), 0) <= 0) {
                        printf("Client on socket %d disconnected\n", sock_fd);
                        close(sock_fd);
                        FD_CLR(sock_fd, &master_set);
                    } else {
                        switch (header.type) {
                            case REQ_CLIENT_READ: {
                                Msg_Filename_Request req;
                                recv(sock_fd, &req, sizeof(req), 0);
                                printf("Got REQ_CLIENT_READ for '%s' from socket %d\n", req.filename, sock_fd);
                                handle_send_file(sock_fd, req.filename);
                                // We are done, close this client's connection
                                close(sock_fd);
                                FD_CLR(sock_fd, &master_set);
                                break;
                            }
                            default:
                                printf("Got unknown command %d from client %d\n", header.type, sock_fd);
                        }
                    }
                }
            } // end if FD_ISSET
        } // end for loop
    } // end while(1)

    return 0; // Unreachable
}