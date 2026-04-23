#ifndef FILE_IO_H
#define FILE_IO_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <../include/config.h>
size_t myRead(char *path, char **buffer, size_t size);
size_t myWrite(char *path, char *buffer, size_t size);

typedef struct {
    char *filePath;
    char *buffer;
    size_t size;
    size_t result;
} inFile_t;

typedef struct {
    char *filePath;
    char *buffer;
    size_t size;
    size_t result;
} outFile_t;

void *inThread(void *arg);
void *outThread(void *arg);

#endif 
