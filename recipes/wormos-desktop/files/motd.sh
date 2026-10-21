# New terminals on the desktop greet the user (see welcome(1)), once per
# terminal: shells started inside it, or by su -, do not repeat it.
case $- in
*i*)
	if [ -n "${WAYLAND_DISPLAY:-}" ] && [ -z "${WORMOS_GREETED:-}" ] &&
	   [ ! -e "$HOME/.hushlogin" ]; then
		/usr/bin/welcome
	fi
	export WORMOS_GREETED=1
	;;
esac
