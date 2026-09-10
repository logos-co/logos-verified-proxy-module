#!/usr/bin/env bash
# Assert libverifproxy.a DEFINES the entry points verifproxy.h declares.
#
# The standard archive gate proves the .a is real mingw COFF; it cannot prove
# the library target produced anything. That is the failure worth catching
# here: upstream's installPhase collects only `-type f -executable`, which for
# a .a and a .h yields an EMPTY $out, and Nim's `--app:staticlib` reaches `ar`
# through a shim this flake installs by hand. Both fail by shipping less, not
# by erroring.
#
# Runs by hand against a native build too:
#   ARCHIVE=result/lib/libverifproxy.a .github/gates/verifproxy-archive.sh
set -euo pipefail

ARCHIVE=${ARCHIVE:-stage/libverifproxy/lib/libverifproxy.a}
OBJDUMP=${OBJDUMP:-objdump}
HEADER=${HEADER:-stage/libverifproxy/include/verifproxy.h}

[ -f "$ARCHIVE" ] || { echo "::error::no archive at $ARCHIVE"; exit 1; }
[ -f "$HEADER" ]  || { echo "::error::no header at $HEADER"; exit 1; }
# Without this the reader's absence arrives as bash's bare 127 and reads as a
# broken archive rather than a broken $OBJDUMP.
command -v "$OBJDUMP" >/dev/null || { echo "::error::objdump not executable: $OBJDUMP"; exit 1; }

# COFF spells a definition as a nonzero section index; `(sec  0)` is undefined.
syms=$("$OBJDUMP" -t "$ARCHIVE")

rc=0
for s in startVerifProxy stopVerifProxy processVerifProxyTasks proxyCall \
         deliverExecutionTransport deliverBeaconTransport; do
  if grep -qE "\(sec +[1-9][0-9]*\).* ${s}\$" <<<"$syms"; then
    echo "ok   $s"
  else
    echo "::error::$s is declared in verifproxy.h but not defined in $ARCHIVE"
    rc=1
  fi
done
exit $rc
