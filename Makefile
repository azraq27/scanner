all:
	gcc -o scanner -O3 -lsqlite3 scanner.c xxhash.c

debug:
	gcc -o scanner -g -O0 -lsqlite3 scanner.c xxhash.c

clean:
	rm scanner scanner.o
