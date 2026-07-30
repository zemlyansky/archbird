CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror

.PHONY: check
check:
	mkdir -p build
	$(CC) $(CFLAGS) -Isrc src/ui/view.c src/service/api.c src/storage/raw.c test/test_view.c -o build/test-view
	./build/test-view
