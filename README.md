# Space Mail

A local-first mail client for a Linux Mint/LMDE desktop, written in C11 with
GTK 3 and XApp. This repository contains an **early, usable foundation**, not
yet a replacement for Thunderbird or KMail.

## What works

- Configure password-authenticated IMAP over TLS and SMTP over STARTTLS or TLS.
  Add an email address, login name, password or app password, hostnames and
  ports. Passwords go through libsecret to the desktop Secret Service; they
  are never written into the app's account configuration. A compatible local
  provider such as KeePassXC can supply the Secret Service when configured
  and unlocked. The app looks up the password when it syncs.
- Download full messages, keep them locally, search sender/subject/text without
  a connection, read text and HTML-only messages as **plain text**, and save
  attachments. HTML is parsed locally; images and other remote content are
  not fetched for the preview.
- Compose plain-text mail with one recipient and one attachment, save a draft,
  or queue it for sending during a later sync. A message whose delivery cannot
  be confirmed stays in the outbox and requires an explicit retry, since it
  may have been accepted by the server already.
- Import `.eml` and mbox; export an individual original `.eml` or all locally
  stored messages as mbox. Imported mail is kept in a local archive.
- Retain downloaded messages when they disappear from the server. **More →
  Clean missing local copies** shows a confirmation before deleting local
  copies that a completed folder scan marked absent. Locally imported mail,
  drafts and locally sent mail are excluded.
- Use **Offline** to stop network operations and cancel an active sync. The
  app syncs when opened and every five minutes while visible when online.
  Notifications, tray icon, background operation after closing the window,
  and login autostart are each off by default and can be set in Preferences.

## Current limits

Only password-based IMAP/SMTP accounts are implemented. Some providers may
require an app password; OAuth is not implemented. The initial IMAP parser
handles common `LIST`, `SELECT`, `UID SEARCH` and `UID FETCH` responses but
needs broader mailbox and server compatibility testing. Messages are previewed
as text, without rich HTML rendering. Replying, forwarding, multiple
recipients, read/unread synchronization, moving messages, editing drafts,
Maildir import/export, account removal and provider discovery are future work.
The local mail database is not encrypted; its directory is private to your
Unix user, so protect your device and your backups accordingly.

## Build

On LMDE/Debian, install the development packages for Meson, Ninja, GTK 3,
XApp, GMime 3, libsecret, SQLite 3, libcurl and libxml2, plus a C compiler:

```sh
sudo apt install build-essential meson ninja-build libgtk-3-dev libxapp-dev \
  libgmime-3.0-dev libsecret-1-dev libsqlite3-dev libcurl4-openssl-dev libxml2-dev
meson setup build
meson compile -C build
meson test -C build --print-errorlogs
./build/space-mail
```

To install the desktop entry and binary, run `sudo meson install -C build`.
Uninstalling the package must leave user mail, exported files, and backups
alone. The app currently has no data deletion command.

## Local data and connections

| Purpose | XDG-compliant location |
| --- | --- |
| Downloaded mail, search index, drafts, outbox | `$XDG_DATA_HOME/org.axionis.SpaceMail/mail.sqlite3` (defaults to `~/.local/share/org.axionis.SpaceMail/`) |
| Accounts (without passwords) and preferences | `$XDG_CONFIG_HOME/org.axionis.SpaceMail/` (defaults to `~/.config/org.axionis.SpaceMail/`) |
| Optional autostart entry | `$XDG_CONFIG_HOME/autostart/org.axionis.SpaceMail.desktop` |
| Password | Your configured local Secret Service collection, managed outside the app |
| User-selected exports and backups | Wherever you save them; never deleted by app uninstall |

The mail directory is created with mode `0700` and its database and account
file with mode `0600`. SQLite may create journal files in the same directory.
To remove app data manually, quit the app, remove the two app-specific XDG
directories and optional autostart entry, and delete its entries from the
Secret Service. Exports and backups remain where you placed them.

There is no telemetry, account discovery, or network request for displaying
mail. Online sync connects only to the IMAP/SMTP hosts you entered. The
offline setting is persisted. A desktop Secret Service may prompt to unlock
its collection; its storage and unlock policy belong to the chosen provider.
