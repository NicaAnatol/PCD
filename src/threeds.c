#define _POSIX_C_SOURCE 200809L
#include "../include/proto.h"      // Pentru prototipurile serverului, thread-uri si structurile comune
#include "../include/logging.h"    // Pentru initializarea si folosirea sistemului de logare
#include "../include/config.h"     // Pentru structura de configurare si functiile de incarcare/afisare
#include <bits/getopt_core.h>      // Pentru parsarea argumentelor din linia de comanda cu getopt()
#include <semaphore.h>
#include <pthread.h>

// Calea implicita catre fisierul de configurare al serverului
#define CONFIG_FILE "config/server.conf"

// Declarații externe
extern void *http_main(void *args);
extern void *inotify_thread_func(void *arg);
extern sem_t queue_sem;
extern void *session_monitor(void *arg);
// Bariera pentru sincronizarea thread-urilor la pornire
pthread_barrier_t startup_barrier;

// Variabila globala care retine configuratia curenta a serverului
server_config_t g_config;

// Thread simplu de lucru pentru procesare GEO.
void *geo_worker_main(void *args) {
    (void)args;
    
    // Așteaptă la barieră până când toate thread-urile sunt gata
    pthread_barrier_wait(&startup_barrier);
    
    log_message("[WORKER] Starting geo worker thread");
    
    while (1) sleep(1);
    
    pthread_exit(NULL);
}

// Punctul de intrare al aplicatiei server.
int main(int argc, char *argv[]) {
    pthread_t unixthr, inetthr, workerthr, queuethr, cleanup_thr, inotify_thread;
    int opt;
    
    // Incarca valorile de configurare din fisierul implicit
    load_config(CONFIG_FILE, &g_config);
    
    // Permite suprascrierea portului din configuratie prin argument din linia de comanda
    while ((opt = getopt(argc, argv, "p:")) != -1) {
        if (opt == 'p') g_config.inet_port = atoi(optarg);
    }
    
    // Initializare sistem de logare si mesaj de pornire
    log_init("server.log");
    log_message("[MAIN] Starting Geo Processing Server...");
    
    // Afiseaza configuratia curenta pentru verificare si debug
    print_config(&g_config);
    
    // Creeaza directoarele necesare pentru fisiere temporare si procesare
    mkdir(g_config.geo.temp_dir, 0755);
    mkdir("processing/uploads", 0755);
    mkdir("processing/processing", 0755);
    mkdir("processing/outgoing", 0755);
    
    // Elimina socket-ul Unix vechi, daca exista
    unlink(g_config.unix_socket);
    
    if (init_notify_pipe() < 0) {
        log_message("[MAIN] Failed to initialize notify pipe");
        return 1;
    }
    
    if (sem_init(&queue_sem, 0, 0) != 0) {
        log_message("[MAIN] Failed to initialize queue semaphore");
        return 1;
    }
    
    // Inițializează bariera pentru 6 thread-uri (unix, inet, worker, queue, cleanup, inotify)
    if (pthread_barrier_init(&startup_barrier, NULL, 6) != 0) {
        log_message("[MAIN] Failed to initialize startup barrier");
        return 1;
    }
    
    // Porneste thread-urile principale ale serverului
    pthread_create(&unixthr, NULL, unix_main, (void *)g_config.unix_socket);
    pthread_create(&inetthr, NULL, inet_main, &g_config.inet_port);
    pthread_create(&workerthr, NULL, geo_worker_main, NULL);
    pthread_create(&queuethr, NULL, queue_processor, NULL);
    pthread_create(&cleanup_thr, NULL, completed_task_cleanup, NULL);
    pthread_create(&inotify_thread, NULL, inotify_thread_func, NULL);
    pthread_t httpthr;
    pthread_create(&httpthr, NULL, http_main, NULL);
    pthread_t monitor_thr;
    pthread_create(&monitor_thr, NULL, session_monitor, NULL);
    // Asteapta terminarea thread-urilor
    pthread_join(unixthr, NULL);
    pthread_join(inetthr, NULL);
    pthread_join(workerthr, NULL);
    pthread_join(queuethr, NULL);
    pthread_join(cleanup_thr, NULL);
    pthread_join(inotify_thread, NULL);
    pthread_join(httpthr, NULL);
    pthread_join(monitor_thr, NULL);
    // Distruge bariera și semaforul
    pthread_barrier_destroy(&startup_barrier);
    sem_destroy(&queue_sem);
    
    // Curata socket-ul Unix la inchiderea serverului
    unlink(g_config.unix_socket);
    log_message("[MAIN] Server shutdown");
    
    return 0;
}