#include "../include/logging.h"   // Declaratii pentru functiile sistemului de logging
#include <time.h>                 // Pentru time(), localtime() si strftime()
#include <string.h>               // Pentru strlen()
#include <unistd.h>               // Pentru write() si close()
#include <fcntl.h>                // Pentru open() si flag-urile asociate fisierelor
#include <pthread.h>              // Pentru mutex-ul folosit la sincronizarea accesului la log
#include <stdio.h>                // Pentru snprintf()

// Descriptorul fisierului de log.
// Daca deschiderea fisierului esueaza, se va folosi STDOUT.
static int log_fd = -1;

// Mutex folosit pentru a preveni scrierea simultana in log din mai multe thread-uri.
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

// Initializeaza sistemul de logging prin deschiderea fisierului primit ca parametru.
// Daca exista deja un fisier de log deschis, acesta este inchis inainte de redeschidere.
void log_init(const char *filename) {
    if (log_fd >= 0) close(log_fd);
    
    // Deschide fisierul de log in mod append, astfel incat mesajele noi sa fie adaugate la final
    log_fd = open(filename, O_WRONLY | O_CREAT | O_APPEND, 0644);
    
    // Daca fisierul nu poate fi deschis, se va scrie pe iesirea standard
    if (log_fd < 0) log_fd = STDOUT_FILENO;
}

// Scrie un mesaj in log, precedat de timestamp.
// Accesul este protejat cu mutex pentru a evita amestecarea mesajelor intre thread-uri.
void log_message(const char *msg) {
    if (log_fd < 0) log_fd = STDOUT_FILENO;
    
    // Blocheaza accesul concurent la fisierul de log
    pthread_mutex_lock(&log_mutex);
    
    // Obtine momentul curent si il formateaza pentru afisare
    time_t now = time(NULL);
    char timebuf[64];
    strftime(timebuf, sizeof(timebuf), "[%Y-%m-%d %H:%M:%S] ", localtime(&now));
    
    // Scrie timestamp-ul, mesajul propriu-zis si caracterul newline
    write(log_fd, timebuf, strlen(timebuf));
    write(log_fd, msg, strlen(msg));
    write(log_fd, "\n", 1);
    
    // Deblocheaza accesul pentru alte thread-uri
    pthread_mutex_unlock(&log_mutex);
}

// Construieste un mesaj de log dintr-un prefix text si o valoare intreaga.
void log_int(const char *prefix, int value) {
    char buf[256];
    int len = snprintf(buf, sizeof(buf), "%s %d", prefix, value);
    
    // Daca formatarea a reusit si rezultatul incape in buffer, mesajul este trimis la log
    if (len > 0 && len < (int)sizeof(buf)) {
        log_message(buf);
    }
}

// Construieste un mesaj de log dintr-un prefix text si o valoare de tip double.
void log_double(const char *prefix, double value) {
    char buf[256];
    int len = snprintf(buf, sizeof(buf), "%s %.6f", prefix, value);
    
    // Daca formatarea a reusit si rezultatul incape in buffer, mesajul este trimis la log
    if (len > 0 && len < (int)sizeof(buf)) {
        log_message(buf);
    }
}