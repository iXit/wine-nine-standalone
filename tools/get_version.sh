#!/bin/sh -e

MAJOR=0
MINOR=6
BUILD=0
REVISION=`git rev-list --count HEAD 2>/dev/null || echo "0"`
STAGE="devel"

printf "$MAJOR.$MINOR.$BUILD.$REVISION-$STAGE"

exit 0
