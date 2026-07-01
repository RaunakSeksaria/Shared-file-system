CC=gcc
CFLAGS=-O2 -Wall -Wextra -Werror -pthread -std=c11

SRC_COMMON=src/common/net.c src/common/log.c src/common/protocol.c src/common/errors.c src/common/acl.c
SRC_SS=src/ss/file_scan.c src/ss/file_storage.c src/ss/sentence_parser.c src/ss/runtime_state.c src/ss/write_session.c
SRC_NM=src/nm/index.c src/nm/access_control.c src/nm/commands.c src/nm/registry.c src/nm/access_requests.c src/nm/heartbeat_monitor.c src/nm/replication.c src/nm/replication_worker.c
SRC_CLIENT=src/client/commands.c
INCLUDES=-Iinclude

all: nm ss client

nm: $(SRC_COMMON) $(SRC_NM) src/nm/main.c
	$(CC) $(CFLAGS) $(INCLUDES) -o bin_nm src/nm/main.c $(SRC_COMMON) $(SRC_NM)

ss: $(SRC_COMMON) $(SRC_SS) src/ss/main.c
	$(CC) $(CFLAGS) $(INCLUDES) -o bin_ss src/ss/main.c $(SRC_COMMON) $(SRC_SS)

client: $(SRC_COMMON) $(SRC_CLIENT) src/client/main.c
	$(CC) $(CFLAGS) $(INCLUDES) -o bin_client src/client/main.c $(SRC_COMMON) $(SRC_CLIENT)

clean:
	rm -f bin_nm bin_ss bin_client

.PHONY: all clean

re:
	make clean
	make all

test:
	uv run --with pytest --with pexpect python -m pytest tests/lint:
	uv run --with ruff ruff check tests/

# Static analysis (advisory): recompile every source under the GCC analyzer.
# Not -Werror: known false positives on bounded strncpy patterns / inline alloc wrappers.
ANALYZE_SRC=$(SRC_COMMON) $(SRC_NM) $(SRC_SS) $(SRC_CLIENT) src/nm/main.c src/ss/main.c src/client/main.c
ANALYZE_FLAGS=-O2 -Wall -Wextra -fanalyzer -pthread -std=c11
analyze:
	@for f in $(ANALYZE_SRC); do $(CC) $(ANALYZE_FLAGS) $(INCLUDES) -c $$f -o /dev/null; done

# Quality gate: warning-clean build (-Werror) + analyzer report + tests + lint.
check: re analyze test lint

# ThreadSanitizer build (concurrency debugging).
tsan: CFLAGS=-O1 -g -Wall -Wextra -pthread -std=c11 -fsanitize=thread
tsan: clean all

.PHONY: lint analyze check tsan asan

asan: CFLAGS=-O1 -g -Wall -Wextra -pthread -std=c11 -fsanitize=address -fno-omit-frame-pointer
asan: clean all
