#include "../protocol/protocol.h"
#include <netinet/in.h>

// This is the info the SS will tell the NM about itself
#define MY_IP_FOR_CLIENTS "127.0.0.1"
#define MY_PORT_FOR_CLIENTS 8082

int main() {
    int sock;
    struct sockaddr_in nm_addr;

    // 1. Create socket to talk to NM
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) error_exit("socket");

    // 2. Set Name Server info (we know this)
    memset(&nm_addr, 0, sizeof(nm_addr));
    nm_addr.sin_family = AF_INET;
    nm_addr.sin_port = htons(NM_PORT);
    if (inet_pton(AF_INET, "127.0.0.1", &nm_addr.sin_addr) <= 0) {
        error_exit("inet_pton: invalid address");
    }

    // 3. Connect to Name Server
    if (connect(sock, (struct sockaddr *)&nm_addr, sizeof(nm_addr)) < 0) {
        error_exit("connect");
    }
    printf("Storage Server connected to Name Server...\n");

    // 4. Prepare and send registration message
    Header header;
    header.type = REQ_SS_REGISTER;
    header.payload_size = sizeof(Msg_SS_Register);

    Msg_SS_Register reg_msg;
    strncpy(reg_msg.ss_ip, MY_IP_FOR_CLIENTS, MAX_IP_LEN);
    reg_msg.client_port = MY_PORT_FOR_CLIENTS;
    
    // Send header, then payload
    if (send(sock, &header, sizeof(Header), 0) < 0) error_exit("send header");
    if (send(sock, &reg_msg, sizeof(reg_msg), 0) < 0) error_exit("send payload");
    
    // 5. Wait for an "OK" response
    Header res_header;
    if (recv(sock, &res_header, sizeof(Header), 0) < 0) error_exit("recv response");

    if (res_header.type == RES_OK) {
        printf("Registration successful!\n");
    } else {
        printf("Registration failed.\n");
    }

    // 6. Close and exit (for now)
    // In the real project, you would stay connected
    // AND you would start *another* server loop here to listen for clients
    close(sock);
    return 0;
}