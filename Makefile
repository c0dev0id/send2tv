CC ?= cc
CFLAGS = -Wall -Wextra -O2
PKG_CFLAGS != pkg-config --cflags libavformat libavcodec libavutil \
                  libavdevice libavfilter libswscale libswresample
CFLAGS += ${PKG_CFLAGS}
PKG_LIBS != pkg-config --libs libavformat libavcodec libavutil \
                  libavdevice libavfilter libswscale libswresample
LDFLAGS = ${PKG_LIBS}
LDFLAGS += -lpthread

SRC = send2tv.c upnp.c httpd.c media.c
OBJ = ${SRC:.c=.o}

send2tv: ${OBJ}
	${CC} -o $@ ${OBJ} ${LDFLAGS}

.c.o:
	${CC} ${CFLAGS} -c $<

clean:
	rm -f send2tv ${OBJ}

.PHONY: clean
