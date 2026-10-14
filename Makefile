CC = gcc
CFLAGS = -std=gnu11 -O2 -Wall -Wextra
TARGET = vfs
IMAGE = disk.img

.PHONY: all format shell test clean
all: $(TARGET)

$(TARGET): vfs.c
	$(CC) $(CFLAGS) $< -o $@

format: $(TARGET)
	./$(TARGET) mkfs $(IMAGE) 16 512

shell: $(TARGET)
	./$(TARGET) $(IMAGE) shell

test: $(TARGET)
	bash test.sh

clean:
	rm -f $(TARGET) test_disk.img test_input.txt test_output.txt
