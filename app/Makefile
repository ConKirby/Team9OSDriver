CC = gcc
CFLAGS = -Wall -Wextra -pthread
LDFLAGS = -lncurses -lpthread

TARGET = echo_app
SRCS = echo_app.c echo_visualizer.c echo_controller.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
