#include "sos.h"

void _start() {
    print("\n========================================\n");
    print("      sOS Ring 3 UDP Chat Client        \n");
    print("========================================\n\n");

    /* 1. Open socket */
    int sock = socket();
    if (sock < 0) {
        print("[ERROR] Failed to allocate socket!\n");
        exit(1);
    }
    print("[INFO] Socket created successfully.\n");

    /* 2. Bind to local port 8080 */
    bind(sock, 8080);
    print("[INFO] Bound to local port 8080.\n");

    /* 3. Send a message to the Host Machine */
    /* Target IP is VirtualBox Host-Only default IP: 192.168.56.1 */
    unsigned char target_ip[4] = {192, 168, 56, 1}; 
    char* msg = "Hello from sOS! (Send me a message back)\n";
    
    sendto(sock, target_ip, 9090, msg, strlen(msg));
    print("[INFO] Message sent to 192.168.56.1:9090\n\n");

    /* 4. Wait for a response */
    print("Waiting for response from host...\n");
    unsigned char buf[128];
    
    while (1) {
        int len = recvfrom(sock, buf, sizeof(buf) - 1);
        if (len > 0) {
            buf[len] = '\0';
            print("\n>> Received Data: ");
            print((char*)buf);
            print("\n");
            break; /* Exit after first message */
        }
        sleep(50); /* Yield/sleep to avoid 100% CPU lock while polling */
    }

    print("\nNetwork Test Complete.\n");
    exit(0);
}