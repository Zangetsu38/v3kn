# v3kn

v3kn is a self-hosted network server for Vita3K, providing cloud storage and
social features for emulator users.

## Features

- **Accounts and profiles:** account creation and login, online ID and password
  changes, an about-me message, avatars, and profile panels.
- **Cloud storage:** upload and download game saves and trophy data, with a
  default storage quota of 50 MiB per account.
- **Game data:** storage of game title information and trophy configuration data.
- **Friends and presence:** search for players, manage friend requests and
  blocked users, view profiles, and receive presence updates.
- **Activities:** share activities and interact with likes and comments.
- **Messaging:** server-side conversations and messages are experimental and
  untested; the Vita3K messaging interface is not yet functional.
- **Linux updates:** automatically check for and install continuous releases.

## Running the server

Continuous Linux builds for x86-64 and ARM64 are available on the
[releases page](https://github.com/Zangetsu38/v3kn/releases/tag/continuous).
Extract the archive into a writable installation directory. Run the server from
that directory so it can find its files and store user data:

```bash
mkdir -p v3kn
./v3kn.bin
```

The server listens on port **3000** on all network interfaces. Opening
`http://127.0.0.1:3000/` on the host should display the server status page.
On Windows, create the `v3kn` data directory and run `v3kn.exe` from its output
directory instead:

```powershell
New-Item -ItemType Directory -Force v3kn
.\v3kn.exe
```

The server itself uses HTTP, not HTTPS. If exposing it to clients over a
network, use a properly configured HTTPS reverse proxy or tunnel and restrict
direct access to port 3000 as appropriate. A proxy or tunnel does not by itself
guarantee protection against every attack or resource-exhaustion scenario.
Savedata and storage identifiers used in paths are checked to reject path
separators and other unsafe components. For savedata info, upload, and download,
title IDs must be `PCS` followed by one uppercase letter and five digits (for
example, `PCSB00100`). Trophy IDs must be `NPWR`, five digits, an underscore,
and two digits (for example, `NPWR01537_00`).

### Data and logs

- `v3kn/` contains the account database, user profiles, saves, trophies, and
  other server data. Back up this directory to preserve user data.
- `v3kn.log` contains the current server log and is cleared at each startup.
- `logs/` contains dated server logs; `v3kn-update.log` records Linux updates.

Keep account data, authentication tokens, and logs private. New accounts and
password changes receive a Base64-encoded token generated from 32
cryptographically random bytes. Existing tokens remain valid until the account
password is changed.

### Account storage

`v3kn/users.json` is an index mapping account IDs to their current online IDs.
Each account's data is stored in `v3kn/Users/<online_id>/account.json` and
loaded into memory at startup along with the index. Account changes are saved
to the corresponding file; the index is updated when an account is created,
renamed, or deleted. Stop the server before editing account files manually:
in-memory data can otherwise overwrite your changes.

Back up the entire `v3kn/` directory, including profiles and cloud saves.
Account files contain password hashes, salts, tokens, and IP addresses; protect
the backup as carefully as the live data. On Linux, online IDs that differ only
in capitalization can refer to separate directories.

Before overwriting an account file or the index, the server copies its previous
contents to a sibling file ending in `.previous`. If a file is missing or its
JSON cannot be read, startup tries that copy and logs the restoration. Startup
stops if the index and account files conflict. A restored account can contain
an older password or token. The `.previous` files contain private data too.

These copies are not a complete backup. Writes are not atomic, and a crash
during creation, deletion, or renaming can leave the index and directories out
of sync. If a storage write fails, the server rejects account requests until
restarted; inspect the logs and fix the underlying problem before restarting.

The server requires an account index and does not migrate single-file account
databases. If a `v3kn/users.transaction.json` file exists, startup stops without
changing it; use a server version that supports transaction recovery to finish
the pending operation. The server does not modify `v3kn/users.json.bak`.

## Building from source

The project requires Git, CMake 3.22 or newer, a C++23-capable compiler, and OpenSSL.
Clone the repository with its submodules:

```bash
git clone --recursive https://github.com/Zangetsu38/v3kn.git
cd v3kn
```

If the repository was cloned without submodules, run
`git submodule update --init --recursive` first.

For example, on Linux with Ninja and a recent GCC installed:

```bash
cmake --preset linux-ninja-gnu
cmake --build --preset linux-ninja-gnu-release
cd build/linux-ninja-gnu/bin/Release
mkdir -p v3kn
./v3kn.bin
```

Other Linux and Windows presets are available in `CMakePresets.json`. The build
copies the favicon and, on Linux, the updater script beside the executable.
The compiler and C++ standard library must provide `std::expected`; some Clang
and libstdc++ combinations do not, even when C++23 is enabled.

With PowerShell 7, run the isolated account storage integration tests against a
built executable using the command below. The tests use temporary synthetic
accounts and require local port 3000 to be available.

```powershell
pwsh -NoProfile -File .\tests\account_storage_integration.ps1 -Executable .\build\bin\Release\v3kn.exe
```

On Linux, the additional account storage regression test requires Python 3, a
case-sensitive temporary filesystem, and local port 3000:

```bash
python3 tests/account_storage_linux_test.py build/linux-ninja-gnu/bin/Release/v3kn.bin
```

## Linux updates

Run the server from its installation directory with `update-v3kn.sh` beside
`v3kn.bin`. The server checks the `continuous` release every five minutes. When
an update is found, it stops accepting connections, waits for request handlers
to finish, and hands over to `/bin/bash update-v3kn.sh --handover`.
Automatic updates install the published release, replacing the current binary.

The updater requires Bash, wget, unzip, tee, and writable installation files.
It selects the x86-64 or ARM64 archive for the host and validates the required
files before installation.

- **Automatic update:** `--handover` restarts the binary in the existing screen
  session or service without killing or recreating it.
- **Manual update:** run `bash ./update-v3kn.sh` from outside the `v3kn` screen
  session. This mode requires GNU screen. It stops matching `v3kn` screen
  sessions after downloading and validating the release, installs it, and
  starts the server in a detached screen. It returns after checking that the
  server responds on port 3000. Stop any server running outside screen first.

Update progress is displayed live and appended to `v3kn-update.log`. Temporary
download files are removed on success or failure. A completed installation
also removes the legacy `v3kn_tmp` directory and downloaded release ZIPs, not
the `v3kn/` user data directory.

An update failure exits with a nonzero status; the server may remain stopped
and require a manual restart. Automatic handover does not guarantee readiness:
check `v3kn.log` and the HTTP status page after an update. For a manually
started screen session, use `screen -list` and `screen -r v3kn` to inspect it.

## Concurrent requests

The HTTP server uses 128 worker threads and a five-second idle keep-alive
timeout; active long polls have their own timeout. Data access is synchronized
per user or conversation where possible, while account lifecycle operations and
shared database writes require broader coordination.

Long polls and idle connections still occupy workers. The server uses blocking
HTTP requests, so the worker limit is not a guarantee of unlimited concurrency
or protection against resource exhaustion.
