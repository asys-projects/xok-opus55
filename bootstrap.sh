#!/bin/sh
# Regenerate the top-level configure script (NUX ships its own).
set -e
aclocal -I nux/m4 && autoconf
