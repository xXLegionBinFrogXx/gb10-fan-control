#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
arch=$(dpkg --print-architecture)
if [ "$arch" != arm64 ]; then
    printf '%s\n' "gb10-fan packaging requires a native Debian/Ubuntu arm64 host (found $arch); cross-packaging is not supported." >&2
    exit 1
fi

for tool in make dpkg-deb dpkg-shlibdeps; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        printf '%s\n' "Required packaging tool is missing: $tool" >&2
        exit 1
    fi
done

version=$(sed -n 's/^VERSION := //p' "$root/Makefile")
if [ -z "$version" ]; then
    printf '%s\n' "Could not read VERSION from $root/Makefile" >&2
    exit 1
fi

# The version is repeated in the executable, control metadata, DKMS name, and the
# maintainer scripts' dkms arguments. Refuse to package a drifted set: a wrong
# copy silently breaks dkms removal or makes an upgrade invisible to dpkg.
drift=$(grep -REn \
    -e 'VERSION[ :=]+"?[0-9]+\.[0-9]+\.[0-9]+' \
    -e '^Version: [0-9]+\.[0-9]+\.[0-9]+' \
    -e '\-v [0-9]+\.[0-9]+\.[0-9]+' \
    -- "$root/src" "$root/Makefile" "$root/packaging" | grep -Fv -- "$version" || true)
if [ -n "$drift" ]; then
    printf '%s\n' "Version mismatch: expected $version from the Makefile, but found:" >&2
    printf '%s\n' "$drift" >&2
    exit 1
fi

# Rebuild rather than packaging a potentially stale executable.
make -C "$root" -B all
stage=$(mktemp -d "$root/build/.deb.XXXXXX")
trap 'rm -rf -- "$stage"' EXIT
trap 'exit 1' HUP INT TERM
payload="$stage/package"
make -C "$root" install DESTDIR="$payload" PREFIX=/usr
install -d "$payload/DEBIAN" "$stage/debian"

# dpkg-shlibdeps needs a debian/control context, not debhelper or a source package.
printf 'Source: gb10-fan\n\nPackage: gb10-fan\nArchitecture: arm64\n' > "$stage/debian/control"
shlibs=$(cd "$stage" && dpkg-shlibdeps -O -e"$payload/usr/sbin/gb10-fan")
case "$shlibs" in
    shlibs:Depends=*) shlibs=${shlibs#shlibs:Depends=} ;;
    *) printf '%s\n' "dpkg-shlibdeps did not return runtime dependencies" >&2; exit 1 ;;
esac
if [ -z "$shlibs" ]; then
    printf '%s\n' "Refusing to package without derived runtime dependencies" >&2
    exit 1
fi
sed "s/@SHLIBS@/$shlibs/" "$root/packaging/control" > "$payload/DEBIAN/control"
for script in postinst prerm postrm; do
    install -m 0755 "$root/packaging/$script" "$payload/DEBIAN/$script"
done
dpkg-deb --root-owner-group --build "$payload" "$root/build/gb10-fan_${version}_arm64.deb"
