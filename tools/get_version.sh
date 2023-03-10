#!/bin/sh -e

MAJOR=0
MINOR=9
BUILD=0
REVISION=`git rev-list --count HEAD 2>/dev/null || echo "0"`
STAGE="release"

printf "$MAJOR.$MINOR.$BUILD.$REVISION-$STAGE"

exit 0
