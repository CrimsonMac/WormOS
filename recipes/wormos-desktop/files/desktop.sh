# Logging in on the first console starts the desktop; logging out of it
# ends the session. If the desktop cannot start, the shell stays open.
# (wormlogin starts the desktop itself and sets WORMOS_LOGIN.)
if [ "$(tty)" = /dev/tty1 ] && [ -z "${WAYLAND_DISPLAY:-}" ] && [ -z "${WORMOS_LOGIN:-}" ] &&
   [ "$(id -u)" -ne 0 ]; then
	/usr/lib/worm/session && exit
fi
