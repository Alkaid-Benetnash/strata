#! /bin/bash

sudo gdb -p `pgrep filebench | tail -n 1`
