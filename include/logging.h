#ifndef LOGGING_H
#define LOGGING_H

/* Functii de logging low-level (doar write, nu printf) */
void log_init(const char *filename);
void log_message(const char *msg);
void log_int(const char *prefix, int value);
void log_double(const char *prefix, double value);

#endif /* LOGGING_H */
