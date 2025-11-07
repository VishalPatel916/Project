#include "protocol.h"
#include <netinet/in.h>
#include <sys/stat.h> // For mkdir
#include <dirent.h>   // For directory scanning

// This is the info the SS will tell the NM about itself
#define MY_IP_FOR_CLIENTS "127.0.0.1"
#define MY_PORT_FOR_CLIENTS 8082
#define MY_STORAGE_PATH "./ss_storage" // Our persistent storage folder

int main() {
    int sock;
    struct sockaddr_in nm_addr;
    char my_files[MAX_FILES_PER_SS][MAX_FILENAME];
    int file_count = 0;

    // --- 1. Scan our storage directory ---
    printf("Scanning storage directory: %s\n", MY_STORAGE_PATH);
    mkdir(MY_STORAGE_PATH, 0777); // Create directory if it doesn't exist
    
    DIR *d;
    struct dirent *dir;
    d = opendir(MY_STORAGE_PATH);
    if (d) {
        while ((dir = readdir(d)) != NULL && file_count < MAX_FILES_PER_SS) {
            // Skip "." and ".."
            if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0) {
                continue;
            }
            
            strncpy(my_files[file_count], dir->d_name, MAX_FILENAME);
            printf("  -> Found file: %s\n", my_files[file_count]);
            file_count++;
        }
        closedir(d);
    } else {
        error_exit("opendir");
    }
    printf("Found %d files.\n", file_count);


    // --- 2. Create socket to talk to NM ---
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) error_exit("socket");

    // --- 3. Set Name Server info (we know this) ---
    memset(&nm_addr, 0, sizeof(nm_addr));
    nm_addr.sin_family = AF_INET;
    nm_addr.sin_port = htons(NM_PORT);
    if (inet_pton(AF_INET, "127.0.0.1", &nm_addr.sin_addr) <= 0) {
        error_exit("inet_pton: invalid address");
    }

    // --- 4. Connect to Name Server ---
    if (connect(sock, (struct sockaddr *)&nm_addr, sizeof(nm_addr)) < 0) {
        error_exit("connect");
    }
    printf("Storage Server connected to Name Server...\n");

    // --- 5. Prepare and send registration message ---
    Header header;
    header.type = REQ_SS_REGISTER;
    header.payload_size = sizeof(Msg_SS_Register);

    Msg_SS_Register reg_msg;
    strncpy(reg_msg.ss_ip, MY_IP_FOR_CLIENTS, MAX_IP_LEN);
    reg_msg.client_port = MY_PORT_FOR_CLIENTS;
    reg_msg.file_count = file_count; // Tell NM how many files to expect
    
    // Send header, then payload
    if (send(sock, &header, sizeof(Header), 0) < 0) error_exit("send header");
    if (send(sock, &reg_msg, sizeof(reg_msg), 0) < 0) error_exit("send payload");

    // --- 6. Send the file list, one by one ---
    printf("Sending file list to Name Server...\n");
    for (int i = 0; i < file_count; i++) {
        // We don't need a header for these, the NM knows to expect them
        Msg_File_Item item;
        strncpy(item.filename, my_files[i], MAX_FILENAME);
        if (send(sock, &item, sizeof(item), 0) < 0) {
            error_exit("send file item");
        }
    }
    
    // --- 7. Wait for an "OK" response ---
    Header res_header;
    if (recv(sock, &res_header, sizeof(Header), 0) < 0) error_exit("recv response");

    if (res_header.type == RES_OK) {
        printf("Registration successful!\n");
    } else {
        printf("Registration failed.\n");
    }
    
    // --- 8. Stay alive ---
    // In the real project, you would start *another* server loop here
    // to listen for clients on MY_PORT_FOR_CLIENTS
    printf("Storage Server is now online. Press Ctrl+C to exit.\n");
    while (1) {
        sleep(10);
    }

    close(sock);
    return 0;
}