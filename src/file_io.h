#ifndef FILE_IO_H
#define FILE_IO_H

// Pentru functii standard de I/O (folosit mai ales pentru snprintf in implementare)
#include <stdio.h>

// Pentru alocare dinamica si functii utilitare (malloc, free)
#include <stdlib.h>

// Pentru operatii pe stringuri (strlen, strcpy, etc)
#include <string.h>

// Pentru apeluri de sistem (read, write, close)
#include <unistd.h>

// Pentru operatii pe fisiere (open, flag-uri precum O_RDONLY, O_CREAT)
#include <fcntl.h>

// Pentru utilizarea thread-urilor (pthread_create, etc)
#include <pthread.h>

// Pentru structura de configurare globala (daca este folosita in alte module)
#include <../include/config.h>

// Functie pentru citirea unui fisier in buffer folosind apeluri de sistem
// Intoarce numarul de bytes cititi
size_t myRead(char *path, char **buffer, size_t size);

// Functie pentru scrierea unui buffer intr-un fisier folosind apeluri de sistem
// Intoarce numarul de bytes scrisi
size_t myWrite(char *path, char *buffer, size_t size);

// Structura folosita pentru thread-ul de citire din fisier
typedef struct {
    char *filePath;   // calea fisierului de citit
    char *buffer;     // bufferul in care se va salva continutul
    size_t size;      // numarul maxim de bytes de citit
    size_t result;    // numarul efectiv de bytes cititi
} inFile_t;

// Structura folosita pentru thread-ul de scriere in fisier
typedef struct {
    char *filePath;   // calea fisierului de scris
    char *buffer;     // bufferul ce contine datele de scris
    size_t size;      // numarul de bytes de scris
    size_t result;    // numarul efectiv de bytes scrisi
} outFile_t;

// Functie executata de thread pentru citirea fisierului
void *inThread(void *arg);

// Functie executata de thread pentru scrierea fisierului
void *outThread(void *arg);

#endif 