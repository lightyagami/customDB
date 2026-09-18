CC = gcc
CFLAGS = -Wall -Wextra -Iinclude -O2 -fPIC -pthread -D_GNU_SOURCE
SRCS = src/pager.c src/catalog.c src/btree.c src/cursor.c src/parser.c src/vdbe.c src/executor.c src/api.c
OBJS = $(SRCS:.c=.o)
MAIN_OBJ = src/main.o
SERVER_OBJ = src/server.o
TARGET = db
SERVER_TARGET = db_server
LIB_TARGET = libdbms.so

all: $(TARGET) $(SERVER_TARGET) $(LIB_TARGET)

$(TARGET): $(OBJS) $(MAIN_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ -lpthread -lm

$(SERVER_TARGET): $(OBJS) $(SERVER_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ -lpthread -lm

$(LIB_TARGET): $(OBJS)
	$(CC) -shared -fPIC $(CFLAGS) -o $@ $^ -lpthread -lm

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Debug build with debug symbols
debug: CFLAGS = -Wall -Wextra -Iinclude -g -O0 -fPIC -pthread -D_GNU_SOURCE
debug: clean all

# AddressSanitizer & UndefinedBehaviorSanitizer build for memory safety verification
asan: CFLAGS = -Wall -Wextra -Iinclude -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -fPIC -pthread -D_GNU_SOURCE
asan: clean all

# ThreadSanitizer build for multi-threading data race detection
tsan: CFLAGS = -Wall -Wextra -Iinclude -g -O1 -fsanitize=thread -fPIC -pthread -D_GNU_SOURCE
tsan: clean all

clean:
	rm -f src/*.o $(TARGET) $(SERVER_TARGET) $(LIB_TARGET)

.PHONY: all lib clean debug asan tsan
