# Linux update reminders

The managed Linux launcher checks for updates at startup and every six hours
while Noventra remains open. A successfully verified and staged update can send
a desktop notification titled **Noventra update ready**. In builds with the
one-click updater, the message directs the user to **About Noventra → Update and
restart**. Choosing the button closes Noventra normally, installs the verified
update and reopens the regular session. An update never replaces application
files while an installed browser process remains open.

Preview 2 introduces this reminder. Preview 1 does not have reminders and gains
them only after an upgrade. For the first preview 1 to preview 2 update, use
About Noventra for an immediate check or wait for the existing background check.
Published previews before the one-click updater still require closing every
Noventra window and opening it again. A source change does not modify an
already installed package.

## Update and restart

The native About page offers the button only when a verified ready result also
advertises `can_relaunch`. A busy check does not advertise that capability. The
native restart keeps Chromium's session saving and normal beforeunload prompts;
cancelling a prompt cancels the restart. Private browsing sessions are not
restored after restarting.

Linux restarts invoke the adjacent inventoried updater helper with the native
parent PID and Chromium's filtered restart arguments. The helper verifies the
live parent's executable identity and waits for that direct parent to exit
before taking update locks. If it starts after the parent has exited, it does
not attach to a possibly reused PID. This restart helper deliberately survives
its parent; ordinary update checkers remain bound to their owner's lifetime.

A user-requested restart waits up to eight seconds for the retiring checker's
update lock and allows five seconds for remaining installed child processes to
finish. If another check still owns the lock, the healthy old version reopens
with a retry message and the update remains staged. Ordinary startup keeps its
existing nonblocking check behavior. Another running installed browser
instance prevents replacement; it is never terminated. The old version reopens
with the update still staged.
Verification or replacement failures retain the healthy installed version, with
the existing transaction recovery running before it opens.

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
status and its available update action. A desktop notification is not a
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
