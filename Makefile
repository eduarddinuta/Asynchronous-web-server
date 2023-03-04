CFLAGS = -Wall -Wextra -g

build: sock_util.o http_parser.o
		gcc $(CFLAGS) aws.c sock_util.o http_parser.o -o aws -laio

sock_util.o: 
				gcc -c sock_util.c -o sock_util.o

http_parser.o:
				gcc -c http_parser.c -o http_parser.o

clean:
		rm -rf *.o aws