#ifndef LOGGING_H
#define LOGGING_H

// Initializeaza sistemul de logging si deschide fisierul unde se vor scrie mesajele
void log_init(const char *filename);

// Scrie un mesaj simplu in log (text)
void log_message(const char *msg);

// Scrie in log un mesaj format dintr-un prefix si o valoare intreaga
// util pentru debug sau afisarea unor variabile numerice
void log_int(const char *prefix, int value);

// Scrie in log un mesaj format dintr-un prefix si o valoare reala (double)
// folosit pentru valori precum coordonate sau calcule
void log_double(const char *prefix, double value);

#endif