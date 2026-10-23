CC := clang
PYTHON ?= python3
FLAGS := -std=c11 -Wall -Wextra -Werror -Wconversion -Wpedantic -g
CORE := src/format.c src/flash.c
HEADERS := include/fw.h
.PHONY: all test test-unit test-integration test-sanitized demo clean
all: build/fwctl
build:
	mkdir -p build
build/fwctl: $(CORE) src/main.c $(HEADERS) | build
	$(CC) $(FLAGS) -O2 -Iinclude $(CORE) src/main.c -o $@
build/test-core: $(CORE) tests/test_core.c $(HEADERS) | build
	$(CC) $(FLAGS) -O1 -fsanitize=address,undefined -Iinclude $(CORE) tests/test_core.c -o $@
build/fwctl-sanitized: $(CORE) src/main.c $(HEADERS) | build
	$(CC) $(FLAGS) -O1 -fsanitize=address,undefined -Iinclude $(CORE) src/main.c -o $@
test-unit: build/test-core
	./build/test-core
test-integration: all
	$(PYTHON) tests/integration.py --binary build/fwctl --report build/integration-results.json
test-sanitized: build/fwctl-sanitized
	$(PYTHON) tests/integration.py --binary build/fwctl-sanitized --smoke-only --report build/sanitizer-results.json
test: test-unit test-integration test-sanitized
demo: all
	$(PYTHON) tools/demo.py --binary build/fwctl
clean:
	rm -rf build
