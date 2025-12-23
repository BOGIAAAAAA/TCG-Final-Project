CC=gcc
CFLAGS=-O2 -Wall -Wextra -std=c11 -pthread
LDFLAGS=-pthread -lrt

COMMON_OBJ=src/common/proto.o src/common/net.o src/common/ipc.o
COMMON_LIB=libcommon.a

all: server client client_gui

$(COMMON_LIB): $(COMMON_OBJ)
	ar rcs $@ $^

server: src/server.o $(COMMON_LIB)
	$(CC) $(CFLAGS) -o $@ src/server.o $(COMMON_LIB) $(LDFLAGS)

client: src/client.o src/client_app.o $(COMMON_LIB)
	$(CC) $(CFLAGS) -o $@ src/client.o src/client_app.o $(COMMON_LIB) $(LDFLAGS) -lncursesw

client_gui: src/client_gui.o $(COMMON_LIB)
	$(CC) $(CFLAGS) -o $@ src/client_gui.o $(COMMON_LIB) \
	    -lraylib -lm -ldl -lpthread -lrt -lX11


clean:
	rm -f server client client_gui src/*.o src/common/*.o $(COMMON_LIB)

.PHONY: all clean