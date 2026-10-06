#!/bin/sh
# Run the Xok/ExOS system tests (see run-tests.py).
exec python3 "$(dirname "$0")/run-tests.py" "$@"
