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
- Create a user with `create_user <username> <permissions>` and log in with
  `login <username>`. User creation requires starting the server as root;
  system accounts are created without passwords and share the `users` group.
- Use `create`, `chmod`, `move`, `cd`, `list`, `read`, `write`, and `delete`
  after logging in. File and directory permissions use octal notation. Read
  and write accept `-offset=<num>`; `read` streams large files, and interactive
  `write` reads lines until a single `.` is entered.
- Multiple clients are handled in separate server processes. File reads and
  writes use shared and exclusive file locks, respectively.
- Transfer files with `upload <client path> <server path>` and
  `download <server path> <client path>`. Add `-b` to either command to run it
  in the background; `exit` is refused while background jobs are active.
- `list @username[/path]` lists another user's home when its group permissions
  allow access.
- Request a file from another online user with
  `transfer_request <file> <dest_user>`. The recipient is notified in real
  time and can run `accept <directory> <ID>` to receive a copy, or
  `reject <ID>`. Requests also remain queued while the recipient is offline.
- File paths are confined to the logged-in user's home. Absolute paths are
  interpreted from that home; symbolic links are not followed.

## Remaining work

Concurrency and path handling still need broader testing before the project
is complete.

## Constraints

Target Ubuntu 24.04. Use modular C and the system calls and functions covered
in the course; external libraries are not permitted. Do not use `system()`,
`popen()`, or command-line utilities to implement file operations. The server
must run as root to create system users; after login, each client handler drops
to that user's privileges. POSIX message queues coordinate transfer requests.
Minimize time spent running with elevated privileges.

The detailed assignment PDF was provided separately and is not included in
this public repository.
