CFLAGS = -Wall -Wextra -g
DIST   = dist
SRC    = src

all: $(DIST)/server $(DIST)/client

$(DIST)/server: $(SRC)/server.c $(SRC)/util.c $(SRC)/util.h | $(DIST)
	$(CC) $(CFLAGS) -o $@ $(SRC)/server.c $(SRC)/util.c

$(DIST)/client: $(SRC)/client.c $(SRC)/util.c $(SRC)/util.h | $(DIST)
	$(CC) $(CFLAGS) -o $@ $(SRC)/client.c $(SRC)/util.c

$(DIST):
	mkdir -p $(DIST)

clean:
	rm -rf $(DIST)

