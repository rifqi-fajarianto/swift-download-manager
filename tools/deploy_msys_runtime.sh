#!/usr/bin/env bash
set -euo pipefail

# Copies the non-Qt runtime DLLs (MinGW runtime, libcurl and its dependency
# tree, SQLite) plus the CA bundle next to a deployed Windows executable.
# windeployqt only handles the Qt libraries, so a ZIP built without this step
# needs MSYS2 installed on the target machine.
#
# The executable passed in should live in the build tree: the Windows loader
# resolves DLLs from the executable's own directory first, and a fully deployed
# directory would make every dependency look like it came from the toolchain.
#
# CMake starts this script with the Windows PATH, which has neither the MSYS2
# tools nor the MinGW runtime directory; both are added here so the script works
# from an MSYS2 shell and from a plain Windows build.

if [[ $# -ne 2 ]]; then
    echo "usage: deploy_msys_runtime.sh <exe> <deployment-directory>" >&2
    exit 2
fi

prefix="${MINGW_PREFIX:-/ucrt64}"
export PATH="/usr/bin:${prefix}/bin:${PATH}"

executable="$1"
destination="$2"

copied=0
while IFS= read -r dll; do
    [[ -f "$dll" ]] || continue
    # Overwrite so the deployment never keeps a stale runtime from an older
    # toolchain update.
    cp -f "$dll" "$destination/"
    copied=$((copied + 1))
done < <(ldd "$executable" | awk -v prefix="${prefix}/bin/" 'index($0, prefix) {print $3}')

if [[ $copied -eq 0 ]]; then
    echo "error: ldd found no MinGW runtime DLLs for $executable" >&2
    exit 1
fi

# libcurl from MSYS2 defaults to the POSIX CA path /ucrt64/etc/ssl/certs/
# ca-bundle.crt, which does not exist outside an MSYS2 installation, so
# HTTPS would fail with CURLE_SSL_CACERT_BADFILE. The application looks for
# ca-bundle.crt next to the executable (see resolve_ca_bundle); shipping it
# makes the package self-contained.
ca_bundle="${prefix}/etc/ssl/certs/ca-bundle.crt"
if [[ -f "$ca_bundle" ]]; then
    cp -f "$ca_bundle" "$destination/ca-bundle.crt"
else
    echo "warning: CA bundle not found at $ca_bundle; HTTPS will fail on this package" >&2
fi
