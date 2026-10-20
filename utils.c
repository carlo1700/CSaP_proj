#define _POSIX_C_SOURCE 200809L

#include "utils.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int write_all(int fd, const void *data, size_t length)
{
    const unsigned char *position = data;
    size_t written = 0;

    while (written < length) {
        ssize_t result = write(fd, position + written, length - written);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            return -1;
        }
        written += (size_t)result;
    }
    return 0;
}

ssize_t read_retry(int fd, void *buffer, size_t capacity)
{
    ssize_t result;

    do {
        result = read(fd, buffer, capacity);
    } while (result < 0 && errno == EINTR);
    return result;
}

size_t split_arguments(char *line, char **arguments, size_t max)
{
    char *save_pointer = NULL;
    char *token;
    size_t count = 0;

    for (token = strtok_r(line, " \t\r\n", &save_pointer);
         token != NULL && count < max;
         token = strtok_r(NULL, " \t\r\n", &save_pointer)) {
        arguments[count++] = token;
    }
    return count;
}

int parse_offset_option(const char *option, off_t *offset)
{
    char *end;
    long long value;

    if (strncmp(option, "-offset=", 8) != 0 || option[8] == '\0') {
        return -1;
    }
    errno = 0;
    value = strtoll(option + 8, &end, 10);
    if (errno != 0 || *end != '\0' || value < 0) {
        return -1;
    }
    *offset = (off_t)value;
    return 0;
}

int parse_permissions(const char *text, mode_t *permissions)
{
    char *end;
    unsigned long value;
    size_t i;

    if (text == NULL || text[0] == '\0') {
        return -1;
    }
    for (i = 0; text[i] != '\0'; ++i) {
        if (text[i] < '0' || text[i] > '7') {
            return -1;
        }
    }
    errno = 0;
    value = strtoul(text, &end, 8);
    if (errno != 0 || *end != '\0' || value > 0777) {
        return -1;
    }
    *permissions = (mode_t)value;
    return 0;
}

void print_error(const char *message)
{
    printf("ERROR: %s: %s\n", message, strerror(errno));
}
