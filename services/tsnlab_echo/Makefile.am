
pkglib_LTLIBRARIES=tsnlab_echo.la

AM_CPPFLAGS=-I$(top_srcdir)/ -I$(top_srcdir)/include/ -I$(top_builddir)/include/

tsnlab_echo_la_LIBADD = @MODULES_LIBADD@
tsnlab_echo_la_CFLAGS=  @MODULES_CFLAGS@ @OPENSSL_ADD_FLAG@
tsnlab_echo_la_LDFLAGS= -module -avoid-version
tsnlab_echo_la_SOURCES = tsnlab_echo.c


EXTRA_DIST= makefile.w32 tsnlab_echo.def
