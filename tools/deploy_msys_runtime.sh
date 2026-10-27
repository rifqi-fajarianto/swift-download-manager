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

# The CI action installs MSYS2 outside C:\msys64 and exports MINGW_PREFIX as a
# Windows path, so normalise it before building the runtime directory below.
prefix="${MINGW_PREFIX:-/ucrt64}"
if command -v cygpath > /dev/null 2>&1; then
    prefix="$(cygpath -u "$prefix")"
fi
export PATH="/usr/bin:${prefix}/bin:${PATH}"

executable="$1"
destination="$2"

# Walk the PE import table instead of parsing ldd: the paths ldd prints depend
# on the Cygwin and mount configuration of the build machine and have silently
# stopped matching the prefix on the CI runner. objdump reports plain DLL names,
# and each copied DLL is walked again so libcurl's own dependencies follow.
copied=0
declare -A seen=()
pending=("$executable")
while [[ ${#pending[@]} -gt 0 ]]; do
    current="${pending[0]}"
    pending=("${pending[@]:1}")
    while IFS= read -r name; do
        [[ -n "$name" ]] || continue
        [[ -z "${seen[$name]:-}" ]] || continue
        seen[$name]=1
        # DLLs provided by Windows itself are not in the MSYS2 prefix.
        source_dll="${prefix}/bin/${name}"
        [[ -f "$source_dll" ]] || continue
        # Overwrite so the deployment never keeps a stale runtime from an older
        # toolchain update.
        cp -f "$source_dll" "$destination/"
        pending+=("$source_dll")
        copied=$((copied + 1))
    done < <(objdump -p "$current" | awk '$1 == "DLL" && $2 == "Name:" {print $3}')
done

if [[ $copied -eq 0 ]]; then
    echo "error: no MinGW runtime DLLs found for $executable" >&2
    objdump -p "$executable" | awk '$1 == "DLL" && $2 == "Name:" {print "  import: " $3}' >&2
    echo "  prefix: $prefix" >&2
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
