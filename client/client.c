#include "protocol.h"
#include <netinet/in.h>

int main() {
    int sock;
    struct sockaddr_in nm_addr;
    char username[MAX_USERNAME];

    // 1. Get username from user
    printf("Enter your username: ");
    fgets(username, MAX_USERNAME, stdin);
    username[strcspn(username, "\n")] = 0; // Remove trailing newline

    // 2. Create socket
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) error_exit("socket");

    // 3. Set Name Server info (we know this)
    memset(&nm_addr, 0, sizeof(nm_addr));
    nm_addr.sin_family = AF_INET;
    nm_addr.sin_port = htons(NM_PORT);
    if (inet_pton(AF_INET, "127.0.0.1", &nm_addr.sin_addr) <= 0) {
        error_exit("inet_pton: invalid address");
    }

    // 4. Connect to Name Server
    if (connect(sock, (struct sockaddr *)&nm_addr, sizeof(nm_addr)) < 0) {
        error_exit("connect");
    }
    printf("Connected to Name Server...\n");

    // 5. Prepare and send registration message
    Header header;
    header.type = REQ_CLIENT_REGISTER;
    header.payload_size = sizeof(Msg_Client_Register);

    Msg_Client_Register reg_msg;
    strncpy(reg_msg.username, username, MAX_USERNAME);
    
    if (send(sock, &header, sizeof(Header), 0) < 0) error_exit("send header");
    if (send(sock, &reg_msg, sizeof(reg_msg), 0) < 0) error_exit("send payload");
    
    // 6. Wait for an "OK" response
    Header res_header;
    if (recv(sock, &res_header, sizeof(Header), 0) < 0) error_exit("recv response");

    if (res_header.type == RES_OK) {
        printf("Registration successful! Welcome, %s.\n", username);
    } else {
        printf("Registration failed.\n");
        close(sock);
        return 1;
    }

    // 7. Stay alive
    // In Phase 2, this will become the command loop
    printf("Client is now online. Press Ctrl+C to exit.\n");
    while (1) {
        sleep(10);
    }

    close(sock);
    return 0;
}