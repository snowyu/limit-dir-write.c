# limit-dir Makefile

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
PREFIX  ?= /usr/local
BINDIR  := $(PREFIX)/bin

BIN     := limit-dir
SRC     := limit-dir.c

.PHONY: all install uninstall clean check test

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $<

# 单元测试：通过 UNIT_TEST 宏隐藏 main，直接 include 源码
test/unit: test/test_unit.c $(SRC)
	$(CC) $(CFLAGS) -DUNIT_TEST -o $@ test/test_unit.c

check: $(BIN) test/unit
	@./test/unit
	@sh test/run_tests.sh $(CURDIR)/$(BIN)

test: check

clean:
	rm -f $(BIN) test/unit
