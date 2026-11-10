#!/bin/bash

set -euo pipefail

unzip $SHARED_CLAUDE/swall/shared.zip 
unzip $SHARED_CLAUDE/swall/swas.zip
cp swas/* .
cp shared/* .
rm -rf swas shared/
make clean
make all
