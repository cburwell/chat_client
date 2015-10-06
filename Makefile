CC= gcc
CFLAGS= -g -Wall

# LIBS =  -lsocket -lnsl

LIBS =

all:   cclient server

cclient: cclient.c networks.h
	$(CC) $(CFLAGS) -o cclient cclient.c $(LIBS)

server: server.c networks.h
	$(CC) $(CFLAGS) -o server server.c $(LIBS)

clean:
	rm server cclient