#!/bin/sh
# Run this to generate all the initial makefiles, etc.

srcdir=`dirname $0`
test -z "$srcdir" && srcdir=.

DIE=0

(autoconf --version) < /dev/null > /dev/null 2>&1 || {
  echo
  echo "**Error**: You must have \`autoconf' installed."
  echo "Download the appropriate package for your distribution,"
  echo "or get the source tarball at ftp://ftp.gnu.org/pub/gnu/"
  DIE=1
}

(automake --version) < /dev/null > /dev/null 2>&1 || {
  echo
  echo "**Error**: You must have \`automake' (1.14+) installed."
  echo "You can get it from: ftp://ftp.gnu.org/pub/gnu/"
  DIE=1
}

# if no automake, don't bother testing for aclocal
test -n "$NO_AUTOMAKE" || (aclocal --version) < /dev/null > /dev/null 2>&1 || {
  echo
  echo "**Error**: Missing \`aclocal'.  The version of \`automake'"
  echo "installed doesn't appear recent enough."
  echo "You can get automake from ftp://ftp.gnu.org/pub/gnu/"
  DIE=1
}

if test "$DIE" -eq 1; then
  exit 1
fi

if test -z "$*"; then
  echo "**Warning**: I am going to run \`configure' with no arguments."
  echo "If you wish to pass any to it, please specify them on the"
  echo \`$0\'" command line."
  echo
fi

case $CC in
xlc )
  am_opt=--include-deps;;
esac

echo "Running aclocal..."
aclocal -I m4 $ACLOCAL_FLAGS || exit 1

echo "Running autoheader..."
autoheader || exit 1

echo "Running autoconf..."
autoconf || exit 1

echo "Running automake..."
automake --add-missing --copy --foreign || exit 1

if test -z "$NOCONFIGURE"; then
    echo Running $srcdir/configure "$@" ...
    $srcdir/configure "$@" \
    && echo Now type \`make\' to compile. || exit 1
else
    echo Skipping configure process.
fi
