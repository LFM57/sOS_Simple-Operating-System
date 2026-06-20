#include "sos.h"

void _start() {
    print("\n========================================\n");
    print("  TEST DES PERMISSIONS DE FICHIERS R3   \n");
    print("========================================\n\n");

    /* 1. Écriture d'un fichier personnel */
    print("[INFO] Tentative d'ecriture de 'perso.txt'...\n");
    if (write_file("perso.txt", "Ceci est un secret de l'espace utilisateur !")) {
        print("[SUCCES] Fichier cree et ecrit avec succes.\n");
    } else {
        print("[ECHEC] Impossible d'ecrire le fichier (Permission refusee).\n");
    }

    /* 2. Lecture du fichier personnel */
    print("\n[INFO] Tentative de lecture de 'perso.txt'...\n");
    unsigned char read_buf[128];
    unsigned int read_size = 0;
    
    if (read_file("perso.txt", read_buf, &read_size)) {
        read_buf[read_size] = '\0'; /* Null-termination pour affichage */
        print("[SUCCES] Contenu lu : \"");
        print((const char*)read_buf);
        print("\"\n");
    } else {
        print("[ECHEC] Lecture refusee ou fichier absent.\n");
    }

    /* 3. Tentative d'ecriture sur un fichier protege (ex: passwd) */
    print("\n[INFO] Tentative d'ecriture frauduleuse dans 'passwd'...\n");
    if (write_file("/passwd", "hack:00000000:9")) {
        print("[ATTENTION] Ecriture reussie (Vous etes probablement ROOT) !\n");
    } else {
        print("[SANS DANGER] L'OS a bloque l'ecriture (Permission refusee).\n");
    }

    print("\nFin de l'analyse.\n");
    exit(0);
}