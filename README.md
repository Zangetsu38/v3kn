# v3kn
v3kn is a small cloud server designed to store and sync game save data for now.

## Concurrent requests

The HTTP server uses 128 worker threads. Idle keep-alive connections time out
after five seconds; this does not shorten an active long-poll request.
Requests no longer share a global
exclusive request lock: storage, profiles, activities, friendships, and messages
are synchronized per user or conversation. Reads can run together; updates lock
the complete read/modify/write transaction. Friendship operations lock both users
in a consistent order to avoid deadlocks.

Account creation, deletion, renaming (including legacy name repair during a
connection check), and token changes wait for active data operations. Long polls
release this lifecycle lock while waiting and refresh authentication and the
user's directory after waking. Shared databases such as `users.json` still
serialize writes to preserve consistency. Long polling and idle keep-alive
connections still occupy workers; this is a blocking HTTP server, not an
unlimited-concurrency event loop.

Concurrency regression tests use isolated temporary data and loopback HTTP,
without external network requests. Build with `BUILD_TESTING=ON` (the default)
and run `ctest --test-dir build -C Debug -R "^concurrency\." --output-on-failure`
(adjust the build directory and configuration as needed).

## Linux auto-update

Run the server from its installation directory, with `update-v3kn.sh` beside
`v3kn.bin`. The server checks the `continuous` release every five minutes. When
an update is found, it stops accepting connections, waits for request handlers
to finish, and replaces itself with `/bin/bash update-v3kn.sh --handover`. The script does
not need executable permission for this handover.

The updater requires Bash, wget, unzip, tee, and writable installation files.
It selects the x86_64 or ARM64 archive for the host and validates the required
files before stopping an existing server.

- Manual update: run `bash ./update-v3kn.sh` from outside the `v3kn` screen
  session. This mode requires GNU screen. It stops existing sessions named
  `v3kn` after downloading and validating the release, installs it, and starts
  the binary in a detached screen named `v3kn`. It returns to the shell after
  verifying that the screen remains alive and HTTP responds on port 3000.
  Use `screen -r v3kn` to attach. Stop any server running outside screen first.
- Automatic update: the server passes `--handover` after draining requests.
  This mode replaces the updater process with the binary in the existing
  screen session or service, without killing or recreating that session.

Update progress and command output are displayed live and also appended to
`v3kn-update.log`. The temporary download directory is removed when the update
finishes or fails. A completed installation also removes the legacy
`v3kn_tmp`, `ubuntu-aarch64-latest.zip`, and `ubuntu-latest.zip` leftovers; no
user data directories are removed.

An update failure exits with a nonzero status; the server may remain stopped
and require a manual restart after fixing the cause. Automatic handover logs
the restart attempt, not a readiness guarantee: check `v3kn.log` for the new
startup version and verify that the server responds.

After a manual update, use `screen -list` to check the detached `v3kn` session,
`screen -r v3kn` to inspect the server console, and `v3kn-update.log` to review
the installation steps.
