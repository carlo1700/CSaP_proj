# CSaP_proj

Collaborative project for the Computer Systems and Programming course (A.A.
2025-2026).

## Project

Develop a modular C client-server application for Linux that manages isolated
virtual home directories. The server listens on a network socket, supports
multiple concurrent clients, and keeps all file operations within its
configured root directory.

## Modules

| File | Role |
|------|------|
| `server.c` | startup, accept loop, one forked handler per client, console `exit`, reaping |
| `session.c/.h` | one client session: command table and handlers (login, file commands, upload/read/download) |
| `transfer.c/.h` | `transfer_request` / `accept` / `reject` over POSIX message queues |
| `filesystem.c/.h` | sandboxed path resolution and all file operations with `flock` locking |
| `privilege.c/.h` | euid management (root only when strictly needed) |
| `network.c/.h` | length-prefixed frames over TCP, listening and connecting sockets |
| `utils.c/.h` | helpers shared by client and server (I/O retries, argument parsing) |
| `client.c` | interactive shell |
| `client_transfer.c/.h` | client side of upload/download/read and background jobs |

## Compile

    sudo apt install gcc make     # if missing
    make                          # produces ./Server and ./Client
    make clean

## Start the server

    sudo ./Server <root_directory> [IP] [port]     # defaults: 127.0.0.1 8080

- `root_directory` is created if missing; homes live in `<root>/users/<name>`.
- Root is needed by `create_user` (it runs `adduser`) and to switch user at
  login. Right after startup the server keeps root only as saved UID and runs
  with euid `nobody`; it raises the euid just for `create_user`, login and
  shutdown. After `login` the handler permanently becomes the logged user
  (`setuid`), so the kernel enforces that user's permissions.
- Without sudo the server works, but only the account running it can log in
  (its home `<root>/users/<name>` must already exist) and `create_user` fails.
- Type `exit` in the server terminal (or press Ctrl+C) to stop the server and
  all client handlers immediately.

## Start the client

    ./Client [IP] [port]     # defaults: 127.0.0.1 8080

Every command prints `OK: ...` or `ERROR: <reason>`.

## Commands

| Command | Expected output |
|---------|-----------------|
| `create_user <username> <perm>` e.g. `create_user alice 750` | `OK: user created` – creates the system account (no password, group `users`) and the home with the given octal permissions |
| `login <username>` | `OK: logged in` (`ERROR: user does not exist` otherwise) |
| `create <path> <perm>` / `create -d <path> <perm>` | `OK: created` – empty file / directory |
| `chmod <path> <perm>` | `OK: permissions changed` |
| `move <path1> <path2>` | `OK: moved` – if `path2` is a directory the file is moved inside it |
| `cd <path>` | `OK: directory changed` |
| `pwd` | current directory, e.g. `/docs` (`/` is the home) |
| `list [path]` | one line per entry: `rw-r-----	notes.txt	12 bytes` |
| `read [-offset=<n>] <path>` | file content from byte `n` printed on stdout |
| `write [-offset=<n>] <path>` | type the content, end with a line containing only `.`; `OK: file written`. Without `-offset` the file is replaced, with it the data is written from byte `n`. New files get permissions 0700 |
| `delete <path>` | `OK: deleted` (files and empty directories) |
| `upload [-b] <client path> <server path>` | `OK: upload complete`; with `-b`: `Started background upload (pid N)` and later `[Background] Command: upload <server path> <client path> concluded` |
| `download [-b] <server path> <client path>` | `OK: download complete`; with `-b`: `[Background] Command: download <server path> <client path> concluded` |
| `transfer_request <file> <dest_user>` | blocks until `dest_user` decides (also waits for it to log in); then `OK: transfer request accepted; file copied` or `ERROR: recipient rejected the transfer request` |
| `accept <directory> <ID>` | (recipient) `OK: transfer accepted and file copied` |
| `reject <ID>` | (recipient) `OK: transfer request rejected` |
| `exit` | closes the client; refused while background transfers are running |

The recipient of a transfer sees, as soon as it is logged in:

    Transfer request <ID> from alice for file notes.txt. Use: accept <directory> <ID>  |  reject <ID>

`-b` may be written before or after the two paths.

### Paths

Relative and absolute paths, `.` and `..` are accepted. `/` is the user's home,
so `..` never leaves it. Symbolic links are refused. Only `list` can go outside
the home, up to `<root>/users`: `list ..` shows every home and `list ../bob`
(or `list @bob`) lists Bob's home if its **group** permissions allow it (all
users share the group `users`; "other" bits are never needed).

## Concurrency

- One process per client (`fork`); the parent reaps finished handlers with
  `waitpid`, so no zombies are left.
- `read`, `download` and the sender side of transfers take a shared `flock`;
  `write`, `upload`, `delete`, `move`, `chmod` and the copy target take an
  exclusive one. So readers run in parallel, writers wait for each other and
  for readers, and delete counts as a write. `move` locks both paths in a
  fixed order, so it cannot deadlock.
- `transfer_request` uses POSIX message queues: one notification queue per
  user (`/csap_user_<uid>`), plus per-request reply and data queues. Data is
  copied from the sender's handler to the recipient's handler, so neither of
  them needs access to the other user's home.
- Background transfers use a forked client process with its own connection;
  the client tracks and reaps them.
- Every upload/download (also in the foreground) opens its own connection,
  logs in with `login <user> --no-notify` (so it never consumes transfer
  notifications) and starts from the client's current directory (`pwd`).
- A transfer peer that stops responding for 30 seconds is treated as gone.

## Constraints

Target Ubuntu 24.04. No `system()`/`popen()`; the only external program is
`adduser`, run with `fork` + `exec`.
