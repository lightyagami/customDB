CC = gcc
CFLAGS = -Wall -Wextra -Iinclude -O2
SRCS = src/pager.c src/catalog.c src/btree.c src/cursor.c src/parser.c src/vdbe.c src/executor.c src/main.c
OBJS = $(SRCS:.c=.o)
TARGET = db

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
