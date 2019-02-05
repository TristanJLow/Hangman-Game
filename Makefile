# CC = c99
# CFLAGS = -Wall -pedantic -lpthread # Show all reasonable warnings
# LDFLAGS =

all: hangman

hangman: *.c
	gcc server.c -std=c11 -g -lpthread -Wall -pedantic -o server
	gcc client.c -std=c11 -g -lpthread -Wall -pedantic -o client

clean: rm hangman