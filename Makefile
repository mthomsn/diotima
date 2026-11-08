CC ?= cc
CFLAGS = -std=gnu11 -Wall -Wextra -O1 -g -DPROMPTS_DIR='"$(CURDIR)/prompts"'
LIB = src/math.c src/plan.c src/phase.c src/store.c src/sha256.c src/coverage.c src/graph.c vendor/cJSON.c
APP = src/session.c src/mcp.c src/claude.c src/tools.c src/agent.c src/term.c src/extract.c \
      src/jobs.c src/chrome.c src/img.c src/proc.c
LIBS = -lpthread -lm

all: build/dio
build/dio: $(LIB) $(APP) src/main.c $(wildcard src/*.h)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(LIB) $(APP) src/main.c $(LIBS)
test: build/test
	./build/test
build/test: tests/test.c $(LIB) $(APP) $(wildcard src/*.h)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/test.c $(LIB) $(APP) $(LIBS)
clean:
	rm -rf build
.PHONY: all test clean
