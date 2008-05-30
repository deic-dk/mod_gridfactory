#
# Makefile for mod_gridfactory to build as a DSO module
#
SHELL = /bin/sh

MODNAME = gridfactory
MODFILE = mod_${MODNAME}.so
APXS = apxs

SRC2 = mod_${MODNAME}.c
MODFILE2 = mod_${MODNAME}.la
APXS2 = apxs2

PKGFILES = ${SRC2} RELEASE README Makefile

default: all

all: module

module: ${SRC2}
	${APXS2} -o ${MODFILE} -c ${SRC2}

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
