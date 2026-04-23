#include "file_io.h"   // Declaratii pentru functiile de citire/scriere si structurile folosite de thread-uri
#include <errno.h>     // Pentru errno si raportarea erorilor de sistem

// Citeste continutul unui fisier in buffer folosind apeluri de sistem.
// Daca bufferul nu este deja alocat, functia il aloca automat.
// Intoarce numarul total de bytes cititi.
size_t myRead(char *path, char **buffer, size_t size) {
    // Deschide fisierul in mod read-only
    int fd = open(path, O_RDONLY);
    if (fd == -1) {
        // Afiseaza eroarea daca fisierul nu poate fi deschis
        char err[256];
        int len = snprintf(err, sizeof(err), "myRead open error: %s\n", strerror(errno));
        write(STDERR_FILENO, err, len);
        return 0;
    }
    
    // Daca bufferul nu exista, se aloca memorie pentru continutul citit + terminatorul de sir
    if (*buffer == NULL) {
        *buffer = malloc(size + 1);
        if (*buffer == NULL) {
            // Inchide fisierul daca alocarea a esuat
            close(fd);
            return 0;
        }
    }
    
    size_t total_read = 0;
    ssize_t n;
    char *buf_ptr = *buffer;
    
    // Citire in bucati pentru a evita o singura operatie mare de read()
    while (total_read < size) {
        size_t to_read = (size - total_read) > 4096 ? 4096 : (size - total_read);
        n = read(fd, buf_ptr, to_read);
        if (n == -1) {
            // Raporteaza eroarea aparuta la citire si intoarce cat s-a citit pana in acel moment
            char err[256];
            int len = snprintf(err, sizeof(err), "myRead read error: %s\n", strerror(errno));
            write(STDERR_FILENO, err, len);
            close(fd);
            return total_read;
        }
        
        // Daca read() intoarce 0, s-a ajuns la sfarsitul fisierului
        if (n == 0) break;
        
        total_read += n;
        buf_ptr += n;
    }
    
    // Adauga terminator de sir pentru a permite folosirea bufferului si ca text
    (*buffer)[total_read] = '\0';
    
    // Inchide fisierul dupa terminarea citirii
    close(fd);
    return total_read;
}

// Scrie continutul bufferului intr-un fisier folosind apeluri de sistem.
// Fisierul este creat daca nu exista si suprascris daca exista deja.
// Intoarce numarul total de bytes scrisi.
size_t myWrite(char *path, char *buffer, size_t size) {
    // Deschide fisierul pentru scriere; daca exista deja, continutul anterior este sters
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd == -1) {
        // Afiseaza eroarea daca fisierul nu poate fi deschis sau creat
        char err[256];
        int len = snprintf(err, sizeof(err), "myWrite open error: %s\n", strerror(errno));
        write(STDERR_FILENO, err, len);
        return 0;
    }
    
    size_t total_written = 0;
    ssize_t n;
    char *buf_ptr = buffer;
    
    // Scriere in bucati pentru a trata corect cazurile in care write() nu scrie tot dintr-o data
    while (total_written < size) {
        size_t to_write = (size - total_written) > 4096 ? 4096 : (size - total_written);
        n = write(fd, buf_ptr, to_write);
        if (n == -1) {
            // Raporteaza eroarea aparuta la scriere si intoarce cat s-a scris pana atunci
            char err[256];
            int len = snprintf(err, sizeof(err), "myWrite write error: %s\n", strerror(errno));
            write(STDERR_FILENO, err, len);
            close(fd);
            return total_written;
        }
        
        total_written += n;
        buf_ptr += n;
    }
    
    // Inchide fisierul dupa terminarea scrierii
    close(fd);
    return total_written;
}

// Functia executata de thread-ul de intrare.
// Citeste fisierul asociat structurii primite ca argument si salveaza rezultatul in campul result.
void *inThread(void *arg) {
    inFile_t *f = (inFile_t*)arg;
    f->result = myRead(f->filePath, &f->buffer, f->size);
    
    // Afiseaza un mesaj informativ despre numarul de bytes cititi
    char buf[128];
    int len = snprintf(buf, sizeof(buf), "[THREAD] Citit %zu bytes din %s\n", f->result, f->filePath);
    write(STDOUT_FILENO, buf, len);
    
    return f;
}

// Functia executata de thread-ul de iesire.
// Scrie bufferul asociat structurii primite ca argument in fisier si salveaza rezultatul in campul result.
void *outThread(void *arg) {
    outFile_t *f = (outFile_t*)arg;
    f->result = myWrite(f->filePath, f->buffer, f->size);
    
    // Afiseaza un mesaj informativ despre numarul de bytes scrisi
    char buf[128];
    int len = snprintf(buf, sizeof(buf), "[THREAD] Scris %zu bytes in %s\n", f->result, f->filePath);
    write(STDOUT_FILENO, buf, len);
    
    return f;
}