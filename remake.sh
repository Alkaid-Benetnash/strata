#!/bin/sh
set -e
function remake() {
    make -C $1 $2 clean
    make -C $1 $2 -j8
}
for i in ebuild hbuild; do
    remake libfs PREFIX=${i}
    remake kernfs PREFIX=${i}
done
remake shim/libshim
