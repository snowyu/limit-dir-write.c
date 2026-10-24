# limit-dir Makefile

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
PREFIX  ?= /usr/local
BINDIR  := $(PREFIX)/bin

BIN     := limit-dir
SRC     := limit-dir.c
HELPER  := limit-dir-gpg

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

# 安装二进制与 gpg 辅助脚本；install -D 自动创建目标目录，
# 兼容 DESTDIR= 用于打包（如 make install DESTDIR=pkg/）
install: $(BIN) $(HELPER)
	install -Dm755 $(BIN) "$(DESTDIR)$(BINDIR)/$(BIN)"
	install -Dm755 $(HELPER) "$(DESTDIR)$(BINDIR)/$(HELPER)"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/$(BIN)" "$(DESTDIR)$(BINDIR)/$(HELPER)"

clean:
	rm -f $(BIN) test/unit
