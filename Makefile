# limit-dir Makefile

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
PREFIX  ?= /usr/local
BINDIR  := $(PREFIX)/bin

BIN     := limit-dir
SRC     := limit-dir.c

.PHONY: all install uninstall clean test

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $<

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)

# 快速功能自测：验证当前目录可写、外部目录只读
test: $(BIN)
	@set -e; \
	d=$$(mktemp -d); \
	if ( cd "$$d" && $(CURDIR)/$(BIN) sh -c 'echo ok > inside.txt' \
	     && [ -f inside.txt ] ); then \
	    echo "PASS: 当前目录可写"; \
	else \
	    echo "FAIL: 当前目录不可写"; exit 1; \
	fi; \
	if $(CURDIR)/$(BIN) sh -c 'echo bad > /limit-dir-test.txt' 2>/dev/null; then \
	    echo "FAIL: 外部目录竟然可写"; exit 1; \
	else \
	    echo "PASS: 外部目录只读"; \
	fi; \
	if ( cd "$$d" && $(CURDIR)/$(BIN) sh -c 'cat /etc/hostname' \
	     ) >/dev/null 2>&1; then \
	    echo "PASS: 外部文件可读"; \
	else \
	    echo "FAIL: 外部文件不可读"; exit 1; \
	fi; \
	rm -rf "$$d"; \
	echo "全部测试通过"

clean:
	rm -f $(BIN)
