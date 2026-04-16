#include "file_io.h"
#include <errno.h>

size_t myRead(char *path, char **buffer, size_t size) {
    int fd = open(path, O_RDONLY);
    if (fd == -1) {
        char err[256];
        int len = snprintf(err, sizeof(err), "myRead open error: %s\n", strerror(errno));
        write(STDERR_FILENO, err, len);
        return 0;
    }
    
    if (*buffer == NULL) {
        *buffer = malloc(size + 1);
        if (*buffer == NULL) {
            close(fd);
            return 0;
        }
    }
    
    size_t total_read = 0;
    ssize_t n;
    char *buf_ptr = *buffer;
    
    while (total_read < size) {
        size_t to_read = (size - total_read) > 4096 ? 4096 : (size - total_read);
        n = read(fd, buf_ptr, to_read);
        if (n == -1) {
            char err[256];
            int len = snprintf(err, sizeof(err), "myRead read error: %s\n", strerror(errno));
            write(STDERR_FILENO, err, len);
            close(fd);
            return total_read;
        }
        if (n == 0) break;
        total_read += n;
        buf_ptr += n;
    }
    (*buffer)[total_read] = '\0';
    close(fd);
    return total_read;
}

size_t myWrite(char *path, char *buffer, size_t size) {
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd == -1) {
        char err[256];
        int len = snprintf(err, sizeof(err), "myWrite open error: %s\n", strerror(errno));
        write(STDERR_FILENO, err, len);
        return 0;
    }
    
    size_t total_written = 0;
    ssize_t n;
    char *buf_ptr = buffer;
    
    while (total_written < size) {
        size_t to_write = (size - total_written) > 4096 ? 4096 : (size - total_written);
        n = write(fd, buf_ptr, to_write);
        if (n == -1) {
            char err[256];
            int len = snprintf(err, sizeof(err), "myWrite write error: %s\n", strerror(errno));
            write(STDERR_FILENO, err, len);
            close(fd);
            return total_written;
        }
        total_written += n;
        buf_ptr += n;
    }
    close(fd);
    return total_written;
}

void *inThread(void *arg) {
    inFile_t *f = (inFile_t*)arg;
    f->result = myRead(f->filePath, &f->buffer, f->size);
    
    char buf[128];
    int len = snprintf(buf, sizeof(buf), "[THREAD] Citit %zu bytes din %s\n", f->result, f->filePath);
    write(STDOUT_FILENO, buf, len);
    
    return f;
}

void *outThread(void *arg) {
    outFile_t *f = (outFile_t*)arg;
    f->result = myWrite(f->filePath, f->buffer, f->size);
    
    char buf[128];
    int len = snprintf(buf, sizeof(buf), "[THREAD] Scris %zu bytes in %s\n", f->result, f->filePath);
    write(STDOUT_FILENO, buf, len);
    
    return f;
}
