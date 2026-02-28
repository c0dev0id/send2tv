CC ?= cc
CFLAGS = -Wall -Wextra -O2
CFLAGS += $(shell pkg-config --cflags libavformat libavcodec libavutil \
                  libavdevice libavfilter libswscale libswresample)
LDFLAGS = $(shell pkg-config --libs libavformat libavcodec libavutil \
                  libavdevice libavfilter libswscale libswresample)
LDFLAGS += -lpthread

SRC = send2tv.c upnp.c httpd.c media.c
OBJ = $(SRC:.c=.o)

send2tv: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDFLAGS)

.c.o:
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f send2tv $(OBJ)

.PHONY: clean
