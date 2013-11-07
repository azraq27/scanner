all:
	clang -o scanner -O3 -lsqlite3 scanner.c xxhash.c

debug:
	clang -o scanner -g -O0 -lsqlite3 scanner.c xxhash.c

clean:
	rm scanner scanner.o
