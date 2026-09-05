freqtop: freqtop.c
	$(CC) -gdwarf-4 -D_POSIX_C_SOURCE=199309L -std=c99 -Wall -g -o freqtop freqtop.c -lm

clean:
	rm -f ./freqtop

install: freqtop
	install -d ${DESTDIR}/usr/bin
	install -m 755 freqtop ${DESTDIR}/usr/bin/

uninstall:
	sudo rm -f ${DESTDIR}/usr/bin/freqtop

DISTFILES=\
freqtop.c \
freqtop.1 \
Makefile \
README.md \
LICENSE \
images

tarball:
	tar cvzf ../freqtop_1.3.orig.tar.gz ${DISTFILES}

# upload the source package to the author's personal package archive.
packageupload:
	dpkg-buildpackage -S
	dput ppa:b-stolk/ppa ../freqtop_1.3-1_source.changes

