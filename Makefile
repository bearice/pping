TARGET=pping

CC = gcc
CFLAGS ?= -g3 -finline-functions -Wall -Wmissing-prototypes

LD = gcc
LDLIBS += -lev

HEADERS := job.h pping.h log.h
SOURCES := job.c pping.c log.c
OBJECTS = $(addsuffix .o, $(basename $(SOURCES)))

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(LD) $(OBJECTS) $(LDLIBS) -o$(TARGET)

$(SOURCES): $(HEADERS)

%.o: %.c
	$(CC) $(CFLAGS) -c $<

clean:
	rm -rf *.o pping
