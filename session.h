#ifndef SESSION_H
#define SESSION_H

/* Serves one connected client until it disconnects. */
void session_serve(int client_fd, const char *root);

#endif
