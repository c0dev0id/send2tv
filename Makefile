CC ?= cc
CFLAGS = -Wall -Wextra -O2
PKG_CFLAGS != pkg-config --cflags libavformat libavcodec libavutil \
                  libavdevice libavfilter libswscale libswresample
CFLAGS += ${PKG_CFLAGS}
PKG_LIBS != pkg-config --libs libavformat libavcodec libavutil \
                  libavdevice libavfilter libswscale libswresample
LDFLAGS = ${PKG_LIBS}
LDFLAGS += -lpthread

SRC = send2tv.c upnp.c httpd.c media.c dlna.c
OBJ = ${SRC:.c=.o}

send2tv: ${OBJ}
	${CC} -o $@ ${OBJ} ${LDFLAGS}

.c.o:
	${CC} ${CFLAGS} -c $<

tests: tests.c media.c upnp.c dlna.c send2tv.h
	${CC} -Wall -Wextra -O2 -I ffmpeg-8.0.1 -o tests tests.c \
	    -lpthread -Wl,--unresolved-symbols=ignore-all

test: tests
	./tests

clean:
	rm -f send2tv tests ${OBJ}

.PHONY: clean test
