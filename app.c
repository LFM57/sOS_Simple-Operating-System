#include "sos.h"

void _start() {
    print("\n========================================\n");
    print("        sOS WGET (HTTP Download)        \n");
    print("========================================\n\n");

    /* Using a plain-text HTTP test server (example.com doesn't enforce HTTPS) */
    char* target_host = "example.com";
    unsigned char target_ip[4];
    
    print("[1] Resolving "); print(target_host); print("...\n");
    if (!gethostbyname(target_host, target_ip)) {
        print("[ERROR] DNS Failed.\n");
        exit(1);
    }
    
    print("    -> IP: ");
    print_int(target_ip[0]); print("."); print_int(target_ip[1]); print(".");
    print_int(target_ip[2]); print("."); print_int(target_ip[3]); print("\n");

    print("[2] Opening TCP Socket...\n");
    int sock = socket_tcp();
    if (sock < 0) {
        print("[ERROR] Could not open TCP socket.\n");
        exit(1);
    }

    print("[3] Connecting to Port 80 (HTTP)...\n");
    if (connect(sock, target_ip, 80) < 0) {
        print("[ERROR] Connection Timeout or Refused.\n");
        exit(1);
    }
    print("    -> TCP Connection Established!\n");

    print("[4] Sending HTTP GET Request...\n");
    char request[] = "GET / HTTP/1.0\r\nHost: example.com\r\nConnection: close\r\n\r\n";
    send(sock, request, strlen(request));

    print("[5] Waiting for server response...\n\n");
    
    /* FIX: Allocate the large buffer on the heap instead of the stack! */
    char* rx_buf = (char*)malloc(8192);
    if (!rx_buf) {
        print("[ERROR] Out of memory!\n");
        exit(1);
    }
    
    int attempts = 0;
    while (attempts < 100) { /* 100 * 50ms = 5 sec timeout */
        /* Note: we pass 8191 manually since sizeof(rx_buf) is now just the size of a pointer */
        int len = recv(sock, rx_buf, 8191); 
        if (len > 0) {
            rx_buf[len] = '\0'; /* Null-terminate for safe printing */
            print("---------------- SERVER RESPONSE ----------------\n");
            print(rx_buf);
            print("\n-------------------------------------------------\n");
            break;
        }
        sleep(50);
        attempts++;
    }

    if (attempts >= 100) {
        print("[ERROR] Timeout waiting for HTTP response.\n");
    }

    print("\nDownload Complete.\n");
    exit(0);
}