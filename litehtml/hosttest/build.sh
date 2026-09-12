#!/bin/bash
# Host-side build of the litehtml diagnostic harness (macOS clang).
set -e
cd "$(dirname "$0")"
LH=..
OUT=build
mkdir -p $OUT
SRC=$(ls $LH/src/*.cpp | grep -v utf8_strings)
GUMBO=$(ls $LH/src/gumbo/*.c)
CXXFLAGS="-std=c++11 -g -O0 -I$LH/include -I$LH/include/litehtml -I$LH/include/gumbo -I$LH/src/gumbo -Istub -Wall -Wno-unused"
for f in $SRC; do
    o=$OUT/$(basename $f .cpp).o
    if [ ! -f $o ] || [ $f -nt $o ]; then
        clang++ $CXXFLAGS -c $f -o $o
    fi
done
for f in $GUMBO; do
    o=$OUT/$(basename $f .c).o
    if [ ! -f $o ] || [ $f -nt $o ]; then
        clang -g -O0 -Istub -I$LH/include/gumbo -I$LH/src/gumbo -c $f -o $o
    fi
done
clang++ $CXXFLAGS -c main.cpp -o $OUT/main.o
clang -g -O0 -c stubs.c -o $OUT/stubs.o
clang++ $OUT/*.o -o $OUT/lhtest
echo "built $OUT/lhtest"
