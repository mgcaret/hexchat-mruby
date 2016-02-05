#!/bin/bash
RAKE=./mruby/minirake
if [ -r "${RAKE}" ]; then
  ruby $RAKE $*
else
  echo "Put the MRuby source into ./mruby!"
  exit 1
fi

