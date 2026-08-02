# Maintainer: Don <donf@users.noreply.github.com>
# Contributor: Don <donf@users.noreply.github.com>

pkgname=flickrfree
pkgver=1.0.0
pkgrel=1
pkgdesc='Invisible Wayland client that holds an Always-VRR panel at max refresh on a static desktop (KDE system-tray icon + launcher)'
arch=('x86_64' 'aarch64')
url='https://github.com/DonFlymoor/FlickrFree'
license=('MIT')
depends=('wayland' 'systemd-libs')
makedepends=('base-devel' 'wayland' 'wayland-protocols' 'systemd')
source=("FlickrFree-$pkgver.tar.gz::https://github.com/DonFlymoor/FlickrFree/archive/v$pkgver.tar.gz")
sha256sums=('SKIP')
validpgpkeys=()

# The GitHub tag archive extracts to <RepoName>-<ver> (note capital F).
_builddir="FlickrFree-$pkgver"

build() {
    cd "$srcdir/$_builddir"
    make
}

check() {
    cd "$srcdir/$_builddir"
    ./flickrfree -h >/dev/null 2>&1
}

package() {
    cd "$srcdir/$_builddir"
    install -Dm755 flickrfree "$pkgdir/usr/bin/flickrfree"
    install -Dm644 flickrfree.desktop "$pkgdir/usr/share/applications/$pkgname.desktop"
    install -Dm644 LICENSE "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
    install -Dm644 README.md "$pkgdir/usr/share/doc/$pkgname/README.md"
}
