PROG=	jemalloc_mte_test
SRCS=	jemalloc_mte_test.c
MAN=
LIBADD=	pthread

# Keep the intentional invalid accesses in the hardware-fault tests.
CFLAGS+= -fno-builtin

.include <bsd.prog.mk>
