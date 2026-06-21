#include "sos.h"

void _start() {
    print("\n========================================\n");
    print("      sOS DNS & Routing Test App        \n");
    print("========================================\n\n");

    unsigned char target_ip[4];
    print("[INFO] Resolving example.com...\n");
    
    if (gethostbyname("example.com", target_ip)) {
        print("[INFO] Success! IP Address is: ");
        print_int(target_ip[0]); print(".");
        print_int(target_ip[1]); print(".");
        print_int(target_ip[2]); print(".");
        print_int(target_ip[3]); print("\n");
        
        print("\nBecause of our new Kernel ARP Cache routing, we can ");
        print("even ping this remote server over UDP!");
    } else {
        print("[ERROR] DNS Resolution failed.\n");
    }
    
    print("\n\nTest Complete.\n");
    exit(0);
}