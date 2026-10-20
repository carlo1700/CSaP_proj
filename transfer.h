#ifndef TRANSFER_H
#define TRANSFER_H

#include <limits.h>
#include <stdint.h>
#include <sys/types.h>

#define TRANSFER_CHUNK 4096U
#define TRANSFER_QUEUE_PREFIX "/csap_user_"
#define TRANSFER_REPLY_PREFIX "/csap_reply_"
#define TRANSFER_DATA_PREFIX "/csap_data_"

enum transfer_message_type {
    TRANSFER_REQUEST = 1,
    TRANSFER_ACCEPT,
    TRANSFER_REJECT,
    TRANSFER_DATA,
    TRANSFER_END,
    TRANSFER_RESULT,
    TRANSFER_ERROR
};

typedef struct {
    int type;
    pid_t sender_pid;
    uid_t sender_uid;
    char request_id[64];
    char sender[128];
    char filename[NAME_MAX + 1];
    char reply_queue[64];
    char data_queue[64];
} transfer_request_message;

typedef struct {
    int type;
    union {
        char directory[PATH_MAX];
        struct {
            uint32_t length;
            unsigned char data[TRANSFER_CHUNK];
        } chunk;
        char error[256];
    } payload;
} transfer_reply_message;

#endif
