CC=gcc
CFLAGS=-O2 -Wall -Wextra -Werror -pthread -std=c11

SRC_COMMON=src/common/net.c src/common/log.c src/common/protocol.c src/common/errors.c src/common/acl.c
SRC_SS=src/ss/file_scan.c src/ss/file_storage.c src/ss/sentence_parser.c src/ss/runtime_state.c src/ss/write_session.c
SRC_NM=src/nm/index.c src/nm/access_control.c src/nm/commands.c src/nm/registry.c src/nm/access_requests.c src/nm/heartbeat_monitor.c src/nm/replication.c src/nm/replication_worker.c
SRC_CLIENT=src/client/commands.c
INC_COMMON=-Isrc/common -Isrc/ss -Isrc/nm -Isrc/client

all: nm ss client

nm: $(SRC_COMMON) $(SRC_NM) src/nm/main.c
	$(CC) $(CFLAGS) $(INC_COMMON) -o bin_nm src/nm/main.c $(SRC_COMMON) $(SRC_NM)

ss: $(SRC_COMMON) $(SRC_SS) src/ss/main.c
	$(CC) $(CFLAGS) $(INC_COMMON) -o bin_ss src/ss/main.c $(SRC_COMMON) $(SRC_SS)

client: $(SRC_COMMON) $(SRC_CLIENT) src/client/main.c
	$(CC) $(CFLAGS) $(INC_COMMON) -o bin_client src/client/main.c $(SRC_COMMON) $(SRC_CLIENT)

clean:
	rm -f bin_nm bin_ss bin_client

.PHONY: all clean

re:
	make clean
	make all

test: re
	./run_all.sh