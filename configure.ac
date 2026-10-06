dnl
dnl Xok/ExOS: an exokernel for i386 built on the NUX kernel library.
dnl
dnl NUX is used as a git submodule in ./nux. It is configured and built
dnl as a sub-project (AC_CONFIG_SUBDIRS) in <builddir>/nux, and its make
dnl infrastructure (nux/mk) is reused for the kernel, the library OS and
dnl the user programs.
dnl
AC_INIT([XOK],[1.0])

AC_CONFIG_MACRO_DIRS([nux/m4])

AC_MK_DIR([nux/mk])
AC_NUXROOT_DIR([nux])
AC_LIBEC_DIR([nux/libec])

AC_PREFIX_DEFAULT([${PWD}/install/])

AC_ARG_VAR([ARCH], [The architecture to compile for. Xok supports i386 only.])
if test x"${ARCH}" = x ; then
   ARCH=i386
   ac_configure_args="${ac_configure_args} ARCH=i386"
fi

AS_CASE([${ARCH}],
	[i386], [tool_prefix=i686-unknown-elf],
	[*], [AC_MSG_ERROR([Xok supports only ARCH=i386])])

AC_ARG_VAR([TOOLCHAIN],[toolchain prefix @<:@default: i686-unknown-elf@:>@])
if test x"${TOOLCHAIN}" != x ; then
   tool_prefix="${TOOLCHAIN}"
fi

dnl Host compiler, used for build-time tools (mkxnfs).
AC_PROG_CC
HOST_CC=${CC}
AC_SUBST(HOST_CC)

AC_CHECK_TOOL(TARGET_CC, ${tool_prefix}-gcc, :)
if test x"${TARGET_CC}" = 'x:' ; then
  AC_MSG_ERROR([${tool_prefix}-gcc not found])
fi
AC_CHECK_TOOL(TARGET_LD, ${tool_prefix}-ld, :)
if test x"${TARGET_LD}" = 'x:' ; then
  AC_MSG_ERROR([${tool_prefix}-ld not found])
fi
AC_CHECK_TOOL(TARGET_AR, ${tool_prefix}-ar, :)
if test x"${TARGET_AR}" = 'x:' ; then
  AC_MSG_ERROR([${tool_prefix}-ar not found])
fi
AC_CHECK_TOOL(TARGET_OBJCOPY, ${tool_prefix}-objcopy, :)
if test x"${TARGET_OBJCOPY}" = 'x:' ; then
  AC_MSG_ERROR([${tool_prefix}-objcopy not found])
fi

AC_CHECK_PROG(QEMU, qemu-system-i386, qemu-system-i386, :)

machine=${ARCH}

AC_ARG_ENABLE([opt],
	AS_HELP_STRING([--disable-opt],[compile without compiler optimizations]))
AS_IF([test "x$enable_opt" != "xno"],
	[optflags='-O2 -g'],
	[optflags='-O0 -g'])

AC_ARG_ENABLE([werror],
	AS_HELP_STRING([--disable-werror],[compile ignoring compiler warnings]))
AS_IF([test "x$enable_werror" != "xno"],
	[wflags='-Werror'],
	[wflags=''])

AC_SUBST(MACHINE, ${machine})
AC_SUBST(INSTALLDIR, ${prefix})
AC_SUBST(OPTFLAGS, ["${optflags}"])
AC_SUBST(CONFIGURE_FLAGS, ["${optflags} ${wflags}"])

AC_CONFIG_SUBDIRS([nux])

AC_MK_CONFIG_FILES([
	Makefile
	kern/Makefile
	lib/Makefile
	lib/libc/Makefile
	lib/exos/Makefile
	bin/Makefile
])

AC_CONFIG_FILES([
	tools/Makefile
	mk/xokuser.mk
])

AC_OUTPUT
