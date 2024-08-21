CC ?= gcc
all: kilo
keypress: keypressed.c
	$(CC) keypressed.c -o keypressed -Wall -Wextra -pedantic -g -std=c99
kilo: kilo.c
	$(CC) kilo.c -o kilo -Wall -Wextra -pedantic -g -std=c99
clean: 
	rm kilo
