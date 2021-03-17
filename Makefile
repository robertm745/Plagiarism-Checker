CC = gcc
CFLAGS = -Wall -g -pthread -fsanitize=address 

all: detector 

detector: Asst2.c
	$(CC) $(CFLAGS) Asst2.c -o detector -lm

clean: 
	rm -rf detector *.o 
