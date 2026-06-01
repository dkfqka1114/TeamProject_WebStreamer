CC = gcc
CFLAGS = -Wall -Wextra -O2
LDFLAGS = -lpthread

TARGET = server
OBJS = main.o auth.o stream.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f $(OBJS) $(TARGET)