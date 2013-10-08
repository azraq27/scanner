#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/stat.h>
#include <ftw.h>
#include <unistd.h>
#include <string.h>
#include <sqlite3.h>
#include "xxhash.h"

#define buffer_size	(10*1024*1024)
#define default_min_size (10*1024)

int min_size = default_min_size;

int dups_flag=0,exact_flag=0;

sqlite3 *db;
char hostname[64];

char *create_stmt = "CREATE TABLE IF NOT EXISTS files("
					"computer	VARCHAR,"
					"filename	VARCHAR,"
					"size		INTEGER,"
					"hash		INTEGER"
					")";
					
char *insert_size_stmt = "INSERT INTO files(computer,filename,size) values(?,?,?)";
sqlite3_stmt *insert_size_prep;

char *insert_full_stmt = "INSERT INTO files(computer,filename,size,hash) values(?,?,?,?)";
sqlite3_stmt *insert_full_prep;

char *update_hash_stmt = "UPDATE files set hash=? WHERE rowid=?";
sqlite3_stmt *update_hash_prep;

char *select_stmt = "SELECT rowid FROM files WHERE filename=?";
sqlite3_stmt *select_prep;

int verbose = 0;

unsigned int fast_hash_file(const char *filename, int size) {
	unsigned int hash=0;
	int size_read;
	unsigned char *buffer;
	void *xxhash_state;
	
	buffer = (unsigned char *)malloc(buffer_size+1);
	
	FILE *fp = fopen(filename,"r");
	
	if(fp) {
		xxhash_state = XXH32_init(size);
		size_read = fread(buffer,1,buffer_size,fp);
	
		while(size_read) {
			XXH32_update(xxhash_state,buffer,size_read);
			size_read = fread(buffer,1,buffer_size,fp);
		}
		
		hash = XXH32_digest(xxhash_state);
	}
	
	fclose(fp);
	free(buffer);
	
    return hash;
}

int file_callback(const char *filename, const struct stat *stat_struct, int flags, struct FTW *ftw_struct) {
	unsigned int hash;
	sqlite3_stmt *insert_prep_proxy;
	
	if(stat_struct->st_size < min_size)
		return 0;
	
	char *fullfilename = realpath(filename,0);
	
	sqlite3_bind_text(select_prep,1,fullfilename,strlen(fullfilename),SQLITE_TRANSIENT);
	if(sqlite3_step(select_prep)==SQLITE_DONE) {
		if(verbose) {
			printf("%s: ...",fullfilename);
			fflush(stdout);
		}
		// Only hash every file if we're looking for exact duplicates
		if(exact_flag) {
			hash = fast_hash_file(filename,stat_struct->st_size);
			if(verbose)
				printf("\b\b\b%x\n",hash);
			insert_prep_proxy = insert_full_prep;
			sqlite3_bind_int(insert_prep_proxy,4,hash);
		}
		else {
			insert_prep_proxy = insert_size_prep;
			printf("\n");
		}
		sqlite3_bind_text(insert_prep_proxy,1,hostname,strlen(hostname),SQLITE_STATIC);
		sqlite3_bind_text(insert_prep_proxy,2,fullfilename,strlen(fullfilename),SQLITE_TRANSIENT);
		sqlite3_bind_int(insert_prep_proxy,3,stat_struct->st_size);
		sqlite3_step(insert_prep_proxy);
		sqlite3_reset(insert_prep_proxy);
	}
	
	sqlite3_reset(select_prep);

	free(fullfilename);
	
	return 0;
}

#define max_scan_dirs 10


int main(int argc, char **argv) {
	char output_filename[64] = "output.db";
	int c,i;
	char scan_directories[max_scan_dirs][64];
	int num_scan_dirs = 0;
	int parse_flag=0;
	
	while((c=getopt(argc,argv,"dxpm:o:hv")) != -1) {
		switch(c) {
			case 'd':
				dups_flag = 1;
				break;
			case 'x':
				exact_flag = 1;
				break;
			case 'p':
				parse_flag = 1;
				break;
			case 'm':
				min_size = atoi(optarg);
				break;
			case 'o':
				strncpy(output_filename,optarg,63); output_filename[63]='\0';
				break;
			case 'v':
				verbose = 1;
				break;
			case 'h':
				puts("Scan a bunch of files, hash them, and then figure out if there are any duplicates.\n"
				"\n"
				"Usage: scanner [-x|-d] [-s] [-p] [-m size] [-o output_file.db] [directory]\n"
				"	If directory is given, will default to scanning with no duplicate checking\n"
				"	(just saves the data). If -p is given, or if no directory is given, will parse\n"
				"	the data to find duplicates.\n"
				"\n"	
				"	-x: check if directories given are exact copies of each other\n"
				"		 (results in hashing every single file)\n"
				"\n"
				"	-d: try to find any duplicates anywhere in the given directories\n"
				"		 (will only hash files that have the same size)\n"
				"\n"
				"	-p: force duplicate checking\n"
				"\n"	
				"	-m size: minimum size file to care about (default 10k)\n"
				"\n"	
				"	-o output_file.db: sqlite3 database file to store output in (will append data if present, default 'output.db')\n"
				"\n");
				return 0;
			case '?':
				if(optopt=='m') {
					puts("Error: -m requires an argument");
				}
				else if(optopt=='o') {
					puts("Error: -o requires an argument");
				}
				else {
					printf("Unknown option -%cn\n",optopt);
				}
				return 1;
		}		
	}
	
	for(i=optind; i<argc; i++) {
		if(num_scan_dirs >= max_scan_dirs) {
			printf("Error: maximum number of %d directories can be passed on the command line\n",max_scan_dirs);
			return 1;
		}
		strncpy(scan_directories[num_scan_dirs],argv[optind],63); scan_directories[num_scan_dirs++][63]='\0';
	}
	
	gethostname(hostname,64);
	
	if(verbose)
		printf("Using hostname: %s\n",hostname);
	
	sqlite3_open(output_filename,&db);
	
	if(db==NULL) { 
		puts("Could not load database");
		return 1;
	}

	sqlite3_exec(db,create_stmt,0,0,0);
	sqlite3_prepare_v2(db,insert_size_stmt,strlen(insert_size_stmt),&insert_size_prep,0);
	sqlite3_prepare_v2(db,insert_full_stmt,strlen(insert_full_stmt),&insert_full_prep,0);
	sqlite3_prepare_v2(db,update_hash_stmt,strlen(update_hash_stmt),&update_hash_prep,0);
	sqlite3_prepare_v2(db,select_stmt,strlen(select_stmt),&select_prep,0);
	
	for (i=0;i<num_scan_dirs;i++) {
		if(verbose)
			printf("----\nstarting scan of %s\n\n",scan_directories[i]);
		nftw(scan_directories[i],&file_callback,16,FTW_MOUNT);
	}
	
	if(verbose)
		puts("All done... have a nice day\n");
	
	sqlite3_finalize(insert_size_prep);
	sqlite3_finalize(insert_full_prep);	
	sqlite3_finalize(update_hash_prep);	
	sqlite3_finalize(select_prep);	
	
	return 0;
}