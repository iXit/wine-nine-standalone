#!/bin/sh -e

MAJOR=0
MINOR=1
BUILD=0
REVISION=`git rev-list --count HEAD 2>/dev/null || echo "0"`

echo -n "$MAJOR.$MINOR.$BUILD.$REVISION"

exit 0
