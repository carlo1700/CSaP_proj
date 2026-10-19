# CSaP_proj

Collaborative project for the Computer Systems and Programming course (A.A.
2025-2026).

## Project

Develop a modular C client-server application for Linux that manages isolated
virtual home directories. The server listens on a network socket, supports
multiple concurrent clients, and keeps all file operations within its
configured root directory.

## Current implementation

- Build with `make` on Linux. This produces `Server` and `Client`.
- Start the server with `./Server <root_directory> [IP] [port]`. The default
  address is `127.0.0.1:8080`; the root and its `users` directory are created
  when missing. Stop it with Ctrl+C in the server terminal.
- Start a client with `./Client [IP] [port]`.
- Create a virtual user with `create_user <username> <permissions>` and log in
  with `login <username>`.
- Use `create`, `chmod`, `move`, `cd`, `list`, `read`, `write`, and `delete`
  after logging in. File and directory permissions use octal notation. Read
  and write accept `-offset=<num>`; interactive `write` reads lines until a
  single `.` is entered.
- Multiple clients are handled in separate server processes. File reads and
  writes use shared and exclusive file locks, respectively.
- File paths are confined to the logged-in user's home. Absolute paths are
  interpreted from that home; symbolic links are not followed.

## Remaining work

Uploads, downloads, background transfers, listing other users' homes, real
system-user/group ownership, and the optional accepted transfer-request
feature are not implemented yet. Concurrency and path handling also need
broader testing before the project is complete.

## Constraints

Target Ubuntu 24.04. Use modular C and the system calls and functions covered
in the course; external libraries are not permitted. Do not use `system()`,
`popen()`, or command-line utilities to implement file operations. Minimize
time spent running with elevated privileges.

The detailed assignment PDF was provided separately and is not included in
this public repository.
