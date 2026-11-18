#!/bin/sh
exec python -I "$(dirname "$0")/prepare_guest.py" "$@"
