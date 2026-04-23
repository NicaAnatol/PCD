#ifndef LOGGING_H
#define LOGGING_H


void log_init(const char *filename);
void log_message(const char *msg);
void log_int(const char *prefix, int value);
void log_double(const char *prefix, double value);

#endif 
