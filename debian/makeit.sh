#!/bin/sh
PKG=twofing
VERSION=0.7
DEBVER=1.0

PKGDIR=${PKG}_${VERSION}-${DEBVER}
TARFILE=${PKG}_${VERSION}.orig.tar.gz
FILES="../*.c ../*.h ../Makefile ../70-touchscreen-egalax.rules"

tar -cvzf ${TARFILE} ${FILES}

mkdir ${PKGDIR}
cd ${PKGDIR}
tar -xvzf ../${TARFILE}
cp -ar ../debian .

[ -e debian/changelog ] || dch --create -v ${VERSION} --package ${PKG}
[ -e debian/compat ] || echo 8 > debian/compat

debuild -us -uc 
