TARGET=pping
TARGET_STATIC=pping.static

CC = gcc
CFLAGS ?= -g3 -fPIC -finline-functions -Wall -Wmissing-prototypes

LD = gcc
LDLIBS += -lev

HEADERS := ctx.h pping.h log.h
SOURCES := ctx.c pping.c log.c
OBJECTS = $(addsuffix .o, $(basename $(SOURCES)))

all: binary
binary: $(TARGET)
static: $(TARGET_STATIC)
docker: static Dockerfile
	docker build -t bearice/pping .

$(TARGET): $(OBJECTS)
	$(LD) $(OBJECTS) $(LDLIBS) -o$(TARGET)

$(TARGET_STATIC): $(OBJECTS)
	$(LD) $(OBJECTS) $(LDLIBS) -lm -static -o$@

$(SOURCES): $(HEADERS)

%.o: %.c
	$(CC) $(CFLAGS) -c $<

clean:
	rm -rf $(OBJECTS) $(TARGET) $(TARGET_STATIC)

.PHONY: all clean binary static docker
