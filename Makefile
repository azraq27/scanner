all:
	clang -o scanner -O3 -lsqlite3 src/scanner.c src/xxhash.c

debug:
	clang -o scanner -g -O0 -lsqlite3 src/scanner.c src/xxhash.c

clean:
	rm scanner scanner.o
