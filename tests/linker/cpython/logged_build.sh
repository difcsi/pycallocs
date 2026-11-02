#!/bin/sh
# Records one line per invocation, then runs the real linkstub build command.
log=$1
shift
echo "$*" >> "$log"
exec sh -c "$*"
