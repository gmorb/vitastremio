# Host tests. No Vita toolchain needed. These cover the pure-data logic where
# a silent off-by-one becomes an on-device hang: the AU splitter, the wire
# protocol parser, the address parser, and the SPSC ring buffer.
CC     ?= gcc
CFLAGS := -std=gnu11 -Wall -Wextra -Wshadow -O1 -g

all: run

test_annexb: test_annexb.c ../src/annexb.h
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $@ $<
test_lineproto: test_lineproto.c ../src/lineproto.h fixture.bin
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $@ $<
test_config: test_config.c ../src/config.h
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $@ $<
# thread sanitizer, not address: this one is about ordering, not bounds
test_ring: test_ring.c ../src/ring.h
	$(CC) $(CFLAGS) -fsanitize=thread -pthread -o $@ $<

fixture.bin: ../middleware.py gen_fixture.py
	python3 gen_fixture.py

run: order compile test_annexb test_config test_ring
	@echo "=== annexb ===";  ./test_annexb
	@echo "=== config ===";  ./test_config
	@echo "=== ring ===";    ./test_ring

# The Vita sources can't be compiled here, so a bad search/replace that
# deletes or reorders a function only shows up as a failed on-device build.
# This catches that statically and runs first, since nothing else matters
# if the code won't compile.
order:
	@echo "=== declaration order ==="; python3 check_order.py

clean:
	rm -f test_annexb test_lineproto test_config test_ring fixture.bin
.PHONY: all run clean order compile

# Type-check the Vita sources against stub headers. Catches syntax errors,
# bad signatures, stale struct members and format-string bugs without the
# toolchain. Not a substitute for a real build, but it catches the mistakes
# that were otherwise only found on the device.
# -Werror on the checks that indicate a genuine mistake rather than style.
# implicit-function-declaration in particular is only a warning by default,
# and a missing function definition sailed through this target once because
# of that -- exactly the failure this exists to catch.
CHECK_FLAGS := -std=gnu11 -fsyntax-only -Wall -Wextra \
               -Werror=implicit-function-declaration \
               -Werror=implicit-int \
               -Werror=return-type \
               -Werror=incompatible-pointer-types \
               -Werror=int-conversion \
               -Werror=format \
               -Werror=uninitialized

compile:
	@echo "=== host type-check ==="
	@$(CC) $(CHECK_FLAGS) -Istubs -I../src ../src/player.c && echo "  player.c OK"
	@$(CC) $(CHECK_FLAGS) -Istubs -I../src ../src/main.c && echo "  main.c OK"
