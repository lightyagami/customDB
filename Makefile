CC = gcc
CFLAGS = -Wall -Wextra -Iinclude -O2 -fPIC -pthread
SRCS = src/pager.c src/catalog.c src/btree.c src/cursor.c src/parser.c src/vdbe.c src/executor.c src/api.c
MAIN_SRC = src/main.c
OBJS = $(SRCS:.c=.o)
MAIN_OBJ = src/main.o
TARGET = db
LIB_TARGET = libdbms.so

all: $(TARGET) $(LIB_TARGET)

$(TARGET): $(OBJS) $(MAIN_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ -lpthread

$(LIB_TARGET): $(OBJS)
	$(CC) -shared -fPIC $(CFLAGS) -o $@ $^ -lpthread

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f src/*.o $(TARGET) $(LIB_TARGET)

.PHONY: all lib clean
