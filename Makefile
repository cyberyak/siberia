CC = clang
CFLAGS = -std=c11 -O2 -Wall -Wextra -Werror -Iinclude
FRAMEWORKS = -framework CoreFoundation -framework IOSurface
all: build/libsiberia.a build/test_siberia build/bench
build:
	mkdir -p build
build/siberia.o: src/siberia.c include/siberia.h | build
	$(CC) $(CFLAGS) -c $< -o $@
build/libsiberia.a: build/siberia.o
	libtool -static -o $@ $<
build/test_siberia: test/test_siberia.c build/libsiberia.a
	$(CC) $(CFLAGS) $< build/libsiberia.a $(FRAMEWORKS) -o $@
# The same tests with the race deliberately widened, then with the protection removed.
build/test_yield: test/test_siberia.c src/siberia.c include/siberia.h | build
	$(CC) $(CFLAGS) -DSIBERIA_TEST_YIELD test/test_siberia.c src/siberia.c $(FRAMEWORKS) -o $@
build/test_broken: test/test_siberia.c src/siberia.c include/siberia.h | build
	$(CC) $(CFLAGS) -DSIBERIA_TEST_YIELD -DSIBERIA_TEST_NO_RECHECK test/test_siberia.c src/siberia.c $(FRAMEWORKS) -o $@
test: all build/test_yield build/test_broken
	build/test_siberia
	build/test_yield stress
	build/test_broken expect-tears
.PHONY: all test
build/bench: tools/bench.m build/libsiberia.a
	$(CC) -O2 -Iinclude -fobjc-arc tools/bench.m build/libsiberia.a $(FRAMEWORKS) -framework Foundation -framework Metal -o $@
