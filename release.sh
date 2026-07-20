#!/bin/sh
# Release the DynaJS source code

set -e

version=`cat VERSION`

if [ "$1" = "-h" ] ; then
    echo "release.sh [release_list]"
    echo ""
    echo "release_list: extras binary win_binary cosmo_binary dynajs"

    exit 1
fi

release_list="extras binary win_binary cosmo_binary dynajs"

if [ "$1" != "" ] ; then
    release_list="$1"
fi

#################################################"
# extras

if echo $release_list | grep -w -q extras ; then

d="dynajs-${version}"
name="dynajs-extras-${version}"
outdir="/tmp/${d}"

rm -rf $outdir
mkdir -p $outdir $outdir/unicode $outdir/tests

cp unicode/* $outdir/unicode
cp -a tests/bench-v8 tests/octane tests/cli $outdir/tests

( cd /tmp && tar Jcvf /tmp/${name}.tar.xz ${d} )

fi

#################################################"
# Windows binary release

if echo $release_list | grep -w -q win_binary ; then

# win64

dlldir=/usr/x86_64-w64-mingw32/sys-root/mingw/bin
cross_prefix="x86_64-w64-mingw32-"
d="dynajs-win-x86_64-${version}"
outdir="/tmp/${d}"

rm -rf $outdir
mkdir -p $outdir

make clean
make CONFIG_WIN32=y clean

make CONFIG_WIN32=y CONFIG_LTO=y dynajs.exe
cp dynajs.exe $outdir
${cross_prefix}strip $outdir/dynajs.exe
cp $dlldir/libwinpthread-1.dll $outdir

( cd /tmp/$d && rm -f ../${d}.zip && zip -r ../${d}.zip . )

make CONFIG_WIN32=y clean

# win32

dlldir=/usr/i686-w64-mingw32/sys-root/mingw/bin
cross_prefix="i686-w64-mingw32-"
d="dynajs-win-i686-${version}"
outdir="/tmp/${d}"

rm -rf $outdir
mkdir -p $outdir

make clean
make CONFIG_WIN32=y clean

make CONFIG_WIN32=y CONFIG_M32=y CONFIG_LTO=y dynajs.exe
cp dynajs.exe $outdir
${cross_prefix}strip $outdir/dynajs.exe
cp $dlldir/libwinpthread-1.dll $outdir

( cd /tmp/$d && rm -f ../${d}.zip && zip -r ../${d}.zip . )

fi

#################################################"
# Linux binary release

if echo $release_list | grep -w -q binary ; then

make clean
make CONFIG_WIN32=y clean
make -j4 CONFIG_LTO=y dynajs run-test262
strip dynajs run-test262

d="dynajs-linux-x86_64-${version}"
outdir="/tmp/${d}"

rm -rf $outdir
mkdir -p $outdir

cp dynajs run-test262 $outdir

( cd /tmp/$d && rm -f ../${d}.zip && zip -r ../${d}.zip . )

make clean
make -j4 CONFIG_LTO=y CONFIG_M32=y dynajs run-test262
strip dynajs run-test262

d="dynajs-linux-i686-${version}"
outdir="/tmp/${d}"

rm -rf $outdir
mkdir -p $outdir

cp dynajs run-test262 $outdir

( cd /tmp/$d && rm -f ../${d}.zip && zip -r ../${d}.zip . )

fi

#################################################"
# Cosmopolitan binary release

if echo $release_list | grep -w -q cosmo_binary ; then

export PATH=$PATH:$HOME/cosmocc/bin

d="dynajs-cosmo-${version}"
outdir="/tmp/${d}"

rm -rf $outdir
mkdir -p $outdir

make clean
make CONFIG_COSMO=y -j4 dynajs run-test262
cp dynajs run-test262 $outdir
cp readme-cosmo.txt $outdir/readme.txt

( cd /tmp/$d && rm -f ../${d}.zip && zip -r ../${d}.zip . )

fi

#################################################"
# dynajs

if echo $release_list | grep -w -q dynajs ; then

make build_doc

d="dynajs-${version}"
outdir="/tmp/${d}"

rm -rf $outdir
mkdir -p $outdir $outdir/doc $outdir/tests $outdir/examples

cp Makefile VERSION TODO Changelog readme.txt LICENSE \
   release.sh unicode_download.sh \
   src/dynajs.c src/dynajsc.c repl.js \
   src/dynajs.h src/dynajs-atom.h \
   src/dynajs-libc.c src/dynajs-libc.h src/dynajs-opcode.h \
   src/cutils.c src/cutils.h src/list.h \
   src/libregexp.c src/libregexp.h src/libregexp-opcode.h \
   src/libunicode.c src/libunicode.h src/libunicode-table.h \
   src/dtoa.c src/dtoa.h \
   unicode_gen.c unicode_gen_def.h \
   run-test262.c test262o.conf test262.conf \
   test262o_errors.txt test262_errors.txt \
   $outdir

cp tests/*.js tests/*.patch tests/bjson.c $outdir/tests

cp examples/*.js examples/*.c examples/*.json $outdir/examples

cp doc/dynajs.texi doc/dynajs.pdf doc/dynajs.html \
   $outdir/doc

( cd /tmp && tar Jcvf /tmp/${d}.tar.xz ${d} )

fi
