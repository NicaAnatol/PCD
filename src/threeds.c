#include "../include/proto.h"      // Pentru prototipurile serverului, thread-uri si structurile comune
#include "../include/logging.h"    // Pentru initializarea si folosirea sistemului de logare
#include "../include/config.h"     // Pentru structura de configurare si functiile de incarcare/afisare
#include <bits/getopt_core.h>      // Pentru parsarea argumentelor din linia de comanda cu getopt()

// Calea implicita catre fisierul de configurare al serverului
#define CONFIG_FILE "config/server.conf"

// Variabila globala care retine configuratia curenta a serverului
server_config_t g_config;

// Thread simplu de lucru pentru procesare GEO.
// In varianta actuala functioneaza ca worker de fundal care ramane activ pe toata durata serverului.
void *geo_worker_main(void *args) {
    (void)args;
    
    // Scrie in log pornirea thread-ului worker
    log_message("[WORKER] Starting geo worker thread");
    
    // Bucla infinita pentru mentinerea thread-ului activ
    while (1) sleep(1);
    
    pthread_exit(NULL);
}

// Punctul de intrare al aplicatiei server.
int main(int argc, char *argv[]) {
    pthread_t unixthr, inetthr, workerthr, queuethr;
    int opt;
    
    // Incarca valorile de configurare din fisierul implicit
    load_config(CONFIG_FILE, &g_config);
    
    // Permite suprascrierea portului din configuratie prin argument din linia de comanda
    // Exemplu: ./server -p 18081
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
    
    // Elimina socket-ul Unix vechi, daca exista, pentru a evita erori la restart
    unlink(g_config.unix_socket);
    
    // Porneste thread-urile principale ale serverului:
    // - server Unix socket
    // - server INET/TCP
    // - worker GEO
    // - procesorul pentru coada de task-uri
    pthread_create(&unixthr, NULL, unix_main, (void *)g_config.unix_socket);
    pthread_create(&inetthr, NULL, inet_main, &g_config.inet_port);
    pthread_create(&workerthr, NULL, geo_worker_main, NULL);
    pthread_create(&queuethr, NULL, queue_processor, NULL);
    
    // Asteapta terminarea thread-urilor
    pthread_join(unixthr, NULL);
    pthread_join(inetthr, NULL);
    pthread_join(workerthr, NULL);
    pthread_join(queuethr, NULL);
    
    // Curata socket-ul Unix la inchiderea serverului
    unlink(g_config.unix_socket);
    log_message("[MAIN] Server shutdown");
    
    return 0;
}