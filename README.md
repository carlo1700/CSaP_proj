# CSaP_proj

Collaborative project for the Computer Systems and Programming course (A.A.
2025-2026).

## Project

Develop a modular C client-server application for Linux that manages isolated
virtual home directories. The server listens on a network socket, supports
multiple concurrent clients, and keeps all file operations within its
configured root directory.

## Required functionality

- Create users dynamically and let clients log in without passwords.
- Support file and directory creation, deletion, moving, permission changes,
  directory navigation and listing, reading, writing, uploads and downloads.
- Handle concurrent clients and coordinate conflicting file operations.
- Support background file transfers and clear client-side status and error
  messages.
- Accept relative and absolute paths, including `.` and `..`, while enforcing
  the filesystem boundaries in the assignment.

The optional honors feature is an online, recipient-approved file transfer
request between users.

## Constraints

Target Ubuntu 24.04. Use modular C and the system calls and functions covered
in the course; external libraries are not permitted. Do not use `system()`,
`popen()`, or command-line utilities to implement file operations. Minimize
time spent running with elevated privileges.

The detailed assignment PDF was provided separately and is not included in
this public repository.
