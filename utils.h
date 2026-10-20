#ifndef UTILS_H
#define UTILS_H

#include <stddef.h>
#include <sys/types.h>

#define MAX_ARGUMENTS 6

int write_all(int fd, const void *data, size_t length);
ssize_t read_retry(int fd, void *buffer, size_t capacity);
/* Splits line in place on blanks; returns max when there are more tokens. */
size_t split_arguments(char *line, char **arguments, size_t max);
int parse_offset_option(const char *option, off_t *offset);
int parse_permissions(const char *text, mode_t *permissions);
/* Prints "ERROR: <message>: <errno text>" on stdout. */
void print_error(const char *message);

#endif
