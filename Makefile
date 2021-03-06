
CPPFLAGS=-Werror -Wall
CFLAGS=-g -O2
LDLIBS=-ldl -lunwind
LCOVFLAGS=--no-external

ifeq ($(COVERAGE),1)
  CPPFLAGS += -DGCOV -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=1
  CFLAGS += -fprofile-arcs -ftest-coverage -Og
  LDFLAGS += -fprofile-arcs
endif

all: libfailinj.so libfailinj2.so test test2 test3

libfailinj.so: libfailinj.c
	$(CC) -shared -fPIC $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

libfailinj2.so: libfailinj.c
	$(CC) -shared -fPIC -O2 -DNAME=FAILINJ2 $^ $(LDLIBS) -o $@

coverage.info:
	geninfo $(LCOVFLAGS) . -o $@

clean:
	-rm -f libfailinj.so libfailinj2.so failinj.db test test2 test3 \
		*.gcno *.gcda *.info
