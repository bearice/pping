TARGET=pping

CC = gcc
CFLAGS ?= -g3 -fPIC -finline-functions -Wall -Wmissing-prototypes

LD = gcc
LDLIBS += -lev

HEADERS := ctx.h pping.h log.h
SOURCES := ctx.c pping.c log.c
OBJECTS = $(addsuffix .o, $(basename $(SOURCES)))

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(LD) $(OBJECTS) $(LDLIBS) -o$(TARGET)

$(SOURCES): $(HEADERS)

%.o: %.c
	$(CC) $(CFLAGS) -c $<

clean:
	rm -rf *.o pping
