#
# Makefile for mod_gridfactory to build as a DSO module
#
SHELL = /bin/sh

MODNAME = gridfactory
MODFILE = mod_${MODNAME}.so
SRC2 = mod_${MODNAME}.c
MODFILE2 = mod_${MODNAME}.la
PKGFILES = ${SRC2} RELEASE README Makefile
# You may have to set the two variables below manually
#APXS2=`ls /usr/bin/apxs* /usr/sbin/apxs* 2>/dev/null | head -1`
APXS2=/usr/local/apache2/bin/apxs
APR_VERSION=`apr-1-config --version | sed 's/\.//g'`

default: all

all: module

module: ${SRC2}
	${APXS2} -D APR_VERSION=${APR_VERSION} -o ${MODFILE} -c ${SRC2} # -lmysqlclient -laprutil-1 -lapr-1

install: module
	${APXS2} -i -a -n ${MODNAME} ${MODFILE2}

clean:
	rm -rf *.o *.so *.so.* *.loT *.la *.lo *.slo a.out core core.* pkg .libs

pkg: ${PKGFILES}
	d=${MODNAME}-`cat RELEASE`;			\
	mkdir $$d;					\
	cp -r ${PKGFILES} $$d;				\
	find $$d -name CVS -exec rm -rf '{}' ';';	\
	tar cvzf $$d.tar.gz $$d;			\
	rm -rf $@;					\
	mv $$d $@
