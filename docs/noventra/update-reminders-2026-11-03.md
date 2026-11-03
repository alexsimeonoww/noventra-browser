# Linux update reminders

The managed Linux launcher checks for updates at startup and every six hours
while Noventra remains open. A successfully verified and staged update can send
a desktop notification titled **Noventra update ready**. The message tells the
user to close every Noventra window and open Noventra again. It does not restart
the browser or apply an update while a window remains open.

Preview 2 introduces this reminder. Preview 1 does not have reminders and gains
them only after an upgrade. For the first preview 1 to preview 2 update, use
About Noventra for an immediate check or wait for the existing background check.

## Notification behavior

The reminder runs inside `DesktopUpdater.check()` after signature verification,
anti-replay checks, staged-package verification, feed and signer checks, atomic
replacement checks, and the free-space check. A cached `ready` status does not
send a notification. Checks for the current release and failed checks stay
silent.

The updater records a notification attempt before contacting the desktop. A
private, atomically written ledger limits attempts to one every twelve hours
across browser restarts and different releases. Recording the attempt first
prevents a crash after dispatch from causing another immediate prompt. A failed
attempt also consumes this interval. A clock rollback postpones the next attempt.

The existing updater locks serialize the ledger and notification attempt. No
separate daemon, system service, notification timer, or browser-profile state is
added. Closing the browser stops its existing checker.

Initial cache creation prepares the lock and state in an owned temporary
directory, then publishes the complete cache with one rename. Closing Noventra
during initialization therefore leaves either no cache or a complete cache.
It cannot publish an empty delivery directory that prevents later checks.

## Desktop requirements

Notifications use `/usr/bin/notify-send` and the session's
`org.freedesktop.Notifications` service. The helper is commonly provided by
`libnotify-bin` on Debian and Ubuntu, or `libnotify` on Arch Linux. The desktop
controls placement, styling, notification history, and Do Not Disturb behavior.

The updater removes bundled-library and toolkit overrides before starting the
system helper. The browser's bundled libraries must not override the system
notification client's libraries. The notification uses fixed local text, an
installed icon, and no actions, shell commands, or feed-supplied URLs.

Missing desktop support, failed delivery, a timeout, or an invalid ledger does
not invalidate a staged update. The existing About page still reports its
status and the close-and-reopen instructions. A desktop notification is not a
persistent badge inside Noventra.

## Verification

Run the updater and package regression tests from the project root:

```sh
python3 -B -m unittest discover -s tests/noventra -p test_desktop_updater.py -v
python3 -B -m unittest discover -s tests/noventra -p test_atomic_update_recovery.py -v
python3 -B -m unittest discover -s tests/noventra -p test_package_distribution.py -v
python3 -B -m unittest discover -s tests/noventra -p test_package_installer.py -v
```

The tests use disposable installations and signed loopback feeds. They must not
send notifications to the operator's session bus. The local proof at
`work/update-reminders-2026-11-03/verify_private_notification.py` starts an owned
private bus and notification service, then exercises the real system helper
through the signed update path. That proof requires `dbus-run-session`,
`notify-send`, and system Python with PyGObject.

The proof verifies the notification request. It does not verify how every Linux
desktop renders that request. No personal browser, profile, or notification
service is used.
