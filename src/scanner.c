/*
 Todo:
 
 - finish routines to deal with duplicates
 -- delete
 -- link
 -- how to deal with copies not on this computer?
 
 - add options to change deal with duplicate by command line
 - add "dry run" command
 - add "exclude" command, with defaults
 */

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


#define max_scan_dirs 		10
#define max_filename_size	64

int min_size = default_min_size;

int dups_flag=0,exact_flag=0,hash_flag=0;

sqlite3 *db;
char hostname[max_filename_size];

char *create_stmt = "CREATE TABLE IF NOT EXISTS files("
					"computer	VARCHAR,"
					"filename	VARCHAR,"
					"size		INTEGER,"
					"hash		INTEGER,"
					"dup_check	INTEGER"
					")";

char *create_track_stmt = "CREATE TABLE IF NOT EXISTS track("
						  "size			INTEGER,"
						  "num_files 	INTEGER"
						  ");"
						  "DELETE from track;"
						  "INSERT INTO track(size,num_files) select size,count(*) from FILES GROUP BY size;";

char *select_track_stmt = "SELECT size,num_files FROM track WHERE num_files>1";

char *select_hosts_stmt = "SELECT COUNT(DISTINCT computer) FROM track JOIN files ON track.size=files.size WHERE hash IS NULL AND num_files>1 AND computer!=?";
sqlite3_stmt *select_hosts_prep;

char *select_file_hash_stmt = "SELECT rowid,filename FROM files WHERE size=? AND hash IS NULL";
sqlite3_stmt *select_file_hash_prep;		

char *select_file_dups_count_stmt = "SELECT COUNT(*) FROM files WHERE size=? AND hash IS NOT NULL ORDER BY hash";
sqlite3_stmt *select_file_dups_count_prep;

char *select_file_dups_stmt = "SELECT computer,filename,hash FROM files WHERE size=? AND hash IS NOT NULL ORDER BY hash";
sqlite3_stmt *select_file_dups_prep;

char *update_file_hash_stmt = "UPDATE files set hash=? WHERE rowid=?";
sqlite3_stmt *update_file_hash_prep;

char *insert_size_stmt = "INSERT INTO files(computer,filename,size) values(?,?,?)";
sqlite3_stmt *insert_size_prep;

char *insert_full_stmt = "INSERT INTO files(computer,filename,size,hash) values(?,?,?,?)";
sqlite3_stmt *insert_full_prep;

char *update_hash_stmt = "UPDATE files set hash=? WHERE rowid=?";
sqlite3_stmt *update_hash_prep;

char *select_stmt = "SELECT rowid FROM files WHERE filename=?";
sqlite3_stmt *select_prep;

char *update_reset_dup_check_stmt = "UPDATE files set dup_check=0";
sqlite3_stmt *update_reset_dup_check_prep;

char *update_set_dup_check_stmt = "UPDATE filesset dup_check=1 where rowid=?";
sqlite3_stmt *update_set_dup_check_prep;

char *select_next_filename_stmt = "SELECT rowid,filename,size FROM files WHERE dup_check=0";
sqlite3_stmt *select_next_filename_prep;

char *select_equal_sized_stmt = "SELECT rowid,filename FROM files WHERE size=? AND rowid!=?";
sqlite3_stmt *select_equal_sized_prep;

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
		size_read = (int)fread(buffer,1,buffer_size,fp);
	
		while(size_read) {
			XXH32_update(xxhash_state,buffer,size_read);
			size_read = (int)fread(buffer,1,buffer_size,fp);
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
	
	char *fullfilename = realpath(filename,0);

	if(stat_struct->st_size < min_size || flags!=FTW_F) {
//		if(verbose)
//			printf("%s: (skipping, under min_size)\n",fullfilename);
		return 0;
	}
	
	sqlite3_bind_text(select_prep,1,fullfilename,(int)strlen(fullfilename),SQLITE_TRANSIENT);
	if(sqlite3_step(select_prep)==SQLITE_DONE) {
		if(verbose) {
			printf("%s: ...",fullfilename);
			fflush(stdout);
		}
		// Only hash every file if we're looking for exact duplicates
		if(hash_flag) {
			hash = fast_hash_file(filename,(int)stat_struct->st_size);
			if(verbose)
				printf("\b\b\b%x\n",hash);
			insert_prep_proxy = insert_full_prep;
			sqlite3_bind_int(insert_prep_proxy,4,hash);
		}
		else {
			insert_prep_proxy = insert_size_prep;
			printf("\n");
		}
		sqlite3_bind_text(insert_prep_proxy,1,hostname,(int)strlen(hostname),SQLITE_STATIC);
		sqlite3_bind_text(insert_prep_proxy,2,fullfilename,(int)strlen(fullfilename),SQLITE_TRANSIENT);
		sqlite3_bind_int(insert_prep_proxy,3,(int)stat_struct->st_size);
		sqlite3_step(insert_prep_proxy);
		sqlite3_reset(insert_prep_proxy);
	}
	
	sqlite3_reset(select_prep);

	free(fullfilename);
	
	return 0;
}

void print_dups(char **computers, char **filenames, int num_files, int size) {
	int i;
	
	printf("Found duplicates of %s:%s\n", computers[0],filenames[0]);
	for (i=1;i<num_files;i++)
		printf("\t%s:%s\n",computers[i],filenames[i]);
}

void del_dups(char **computer, char **filenames, int num_files, int size) {
    int i;
    
    printf("Deleting duplicates of %s\n",filenames[0]);
    for(i=1;i<num_files;i++) {
        if(strcmp(computer[i],hostname)==0) {
            if(verbose)
                printf("\tdeleting %s\n",filenames[i]);
            unlink(filenames[i]);
        }
        else {
            printf("Error: cannot deal with duplicate found on %s: %s\n", computer[i],filenames[i]);
        }
    }
}


// void deal_with_dups(char *computer, char **filenames, int num_files, int size)
void (*deal_with_dups)(char **, char **, int, int) = &print_dups;

int dup_checker(void *nothing, int num_cols, char **column_vals, char **column_names) {
	int res,i,num_dups;
	int size = atoi(column_vals[0]);
	int rowid;
	unsigned int hash,new_hash;
	char **filenames;
    char **computers;
	int file_i;
	
	const char *filename,*computer;
	
	printf("checking size %d\n",size);
	sqlite3_bind_text(select_file_hash_prep,1,column_vals[0],(int)strlen(column_vals[0]),SQLITE_STATIC);
	res = sqlite3_step(select_file_hash_prep);
	while(res!=SQLITE_DONE) {
		if(res==SQLITE_ROW) {
			rowid = sqlite3_column_int(select_file_hash_prep,0);
			filename = (const char *)sqlite3_column_text(select_file_hash_prep,1);
		
			hash = fast_hash_file(filename,size);
			if(verbose)
				printf("%s: %x\n",filename,hash);
		
			sqlite3_bind_int(update_file_hash_prep,1,hash);
			sqlite3_bind_int(update_file_hash_prep,2,rowid);
			sqlite3_step(update_file_hash_prep);
			sqlite3_reset(update_file_hash_prep);
		}		
		res = sqlite3_step(select_file_hash_prep);
	}
	
    sqlite3_bind_text(select_file_dups_count_prep,1,column_vals[0],(int)strlen(column_vals[0]),SQLITE_STATIC);
	sqlite3_step(select_file_dups_count_prep);
    num_dups = sqlite3_column_int(select_file_dups_count_prep,0);
    sqlite3_reset(select_file_dups_count_prep);
    
    filenames = malloc(sizeof(char *) * num_dups);
    memset(filenames,0,sizeof(char *) * num_dups);
    
    computers = malloc(sizeof(char *) * num_dups);
    memset(computers,0,sizeof(char *) * num_dups);

	sqlite3_bind_text(select_file_dups_prep,1,column_vals[0],(int)strlen(column_vals[0]),SQLITE_STATIC);
	res = sqlite3_step(select_file_dups_prep);
	hash = 0;
	file_i = 0;
	while(res!=SQLITE_DONE) {
		if(res==SQLITE_ROW) {
			computer = (const char *)sqlite3_column_text(select_file_dups_prep,0);
			filename = (const char *)sqlite3_column_text(select_file_dups_prep,1);
			new_hash = sqlite3_column_int(select_file_dups_prep,2);
			if(hash!=new_hash) {
                if(file_i>1)
                    deal_with_dups(computers, filenames, file_i, size);
			
				for(i=0;i<file_i;i++) {
					if(filenames[i]!=0) {
						free(filenames[i]);
						filenames[i] = 0;
					}
					if(computers[i]!=0) {
						free(computers[i]);
						computers[i] = 0;
					}
				}
				file_i = 0;
				hash = new_hash;
			}
			filenames[file_i] = malloc(strlen(filename)+1);
			computers[file_i] = malloc(strlen(computer)+1);
			strcpy(filenames[file_i],filename);
			strcpy(computers[file_i++],computer);
		}
        res = sqlite3_step(select_file_dups_prep);
	}
    
    if(file_i>1)
        deal_with_dups(computers, filenames, file_i, size);

    for(i=0;i<file_i;i++) {
        if(filenames[i]!=0) {
            free(filenames[i]);
            filenames[i] = 0;
        }
        if(computers[i]!=0) {
            free(computers[i]);
            computers[i] = 0;
        }
    }
	
	sqlite3_reset(select_file_hash_prep);
    sqlite3_reset(select_file_dups_prep);
	free(filenames);
	
	return 0;	
}

void find_duplicates() {
	// Create tracking table and populate with duplicates
	sqlite3_exec(db,create_track_stmt,0,0,0);
	sqlite3_prepare_v2(db,select_hosts_stmt,(int)strlen(select_hosts_stmt),&select_hosts_prep,0);
	sqlite3_bind_text(select_hosts_prep,1,hostname,(int)strlen(hostname),SQLITE_STATIC);
	if(sqlite3_step(select_hosts_prep)==SQLITE_ROW) {
		if(sqlite3_column_int(select_hosts_prep,0) > 0) {
			puts("ERROR: Cannot search for duplicates. There are unhashed files from other\n"
				 "         computers. You have to manually hash all files stored elsewhere\n"
				"         to search for duplicates across multiple computers.");
			sqlite3_finalize(select_hosts_prep);
			return;
		}
	}
	sqlite3_finalize(select_hosts_prep);
	
	sqlite3_prepare_v2(db,select_file_hash_stmt,(int)strlen(select_file_hash_stmt),&select_file_hash_prep,0);
	sqlite3_prepare_v2(db,select_file_dups_stmt,(int)strlen(select_file_dups_stmt),&select_file_dups_prep,0);
	sqlite3_prepare_v2(db,select_file_dups_count_stmt,(int)strlen(select_file_dups_count_stmt),&select_file_dups_count_prep,0);
	sqlite3_prepare_v2(db,update_file_hash_stmt,(int)strlen(update_file_hash_stmt),&update_file_hash_prep,0);
	
	sqlite3_exec(db,select_track_stmt,&dup_checker,0,0);
	
	sqlite3_finalize(select_file_hash_prep);	
	sqlite3_finalize(select_file_dups_prep);
	sqlite3_finalize(select_file_dups_count_prep);
	sqlite3_finalize(update_file_hash_prep);
}

void find_exact() {
	
}

void usage() {
	puts("Scan a bunch of files, hash them, and then figure out if there are any duplicates.\n"
	"\n"
	"Usage: scanner [-x|-d] [-z] [-p] [-m size] [-o output_file.db] [directory]\n"
	"	If directory is given, will default to scanning with no duplicate checking\n"
	"	(just saves the data). If -p is given, will parse the data to find duplicates.\n"
	"\n"	
	"	-x: check if directories given are exact copies of each other\n"
	"		 (results in hashing every single file)\n"
	"\n"
	"	-d: try to find any duplicates anywhere in the given directories\n"
	"		 (will only hash files that have the same size)\n"
	"\n"
	"	-z: force data hashing\n"
	"		  This is necessary if you will be parsing across multiple computers\n"
	"		  since the program has no way of hashing a file on another computer\n"
	"		  when it finally parses the data\n"
    "\n"
    "   -u: unlink duplicates (default is just to print them)\n"
	"\n"
	"	-p: force data parsing: check duplicates or exact matches after the scan\n"
	"\n"	
	"	-m size: minimum size file to care about (default 10k)\n"
	"\n"	
	"	-o output_file.db: sqlite3 database file to store output in \n"
	"		 (will append data if present, default 'output.db')\n"
	"\n"
	"	-v verbose\n"
	"\n"
	"	-h: help\n"
	"\n"
	);
}

int main(int argc, char **argv) {
	char output_filename[max_filename_size] = "output.db";
	int c,i;
	char scan_directories[max_scan_dirs][max_filename_size];
	int num_scan_dirs = 0;
	int parse_flag=0;
	
	if(argc==1) {
		usage();
		return 0;
	}
	
	while((c=getopt(argc,argv,"udxpm:o:hv")) != -1) {
		switch(c) {
			case 'd':
				dups_flag = 1;
				break;
			case 'x':
				exact_flag = 1;
			case 'z':
				hash_flag = 1;
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
            case 'u':
                deal_with_dups = del_dups;
                break;
			case 'v':
				verbose = 1;
				break;
			case 'h':
				usage();
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
	
	if(optind==argc) {
		parse_flag = 1;
	}
	else {
		for(i=optind; i<argc; i++) {
			if(num_scan_dirs >= max_scan_dirs) {
				printf("Error: maximum number of %d directories can be passed on the command line\n",max_scan_dirs);
				return 1;
			}
			strncpy(scan_directories[num_scan_dirs],argv[optind],63); scan_directories[num_scan_dirs++][63]='\0';
		}		
	}
	
	gethostname(hostname,max_filename_size);
	
	if(verbose)
		printf("Using hostname: %s\n",hostname);
	
	sqlite3_open(output_filename,&db);
	
	if(db==NULL) { 
		puts("Could not load database");
		return 1;
	}

	sqlite3_exec(db,create_stmt,0,0,0);
	sqlite3_prepare_v2(db,insert_size_stmt,(int)strlen(insert_size_stmt),&insert_size_prep,0);
	sqlite3_prepare_v2(db,insert_full_stmt,(int)strlen(insert_full_stmt),&insert_full_prep,0);
	sqlite3_prepare_v2(db,update_hash_stmt,(int)strlen(update_hash_stmt),&update_hash_prep,0);
	sqlite3_prepare_v2(db,select_stmt,(int)strlen(select_stmt),&select_prep,0);
	
	for (i=0;i<num_scan_dirs;i++) {
		if(verbose)
			printf("----\nstarting scan of %s\n\n",scan_directories[i]);
		nftw(scan_directories[i],&file_callback,16,FTW_MOUNT);
	}
	
	if(parse_flag) {
		if(dups_flag)
			puts("----\nstarting duplicate check");
			find_duplicates();
		if(exact_flag)
			puts("----\nstarting exact copy check");
			find_exact();
	}
	
	if(verbose)
		puts("\nAll done... have a nice day\n");
	
	sqlite3_finalize(insert_size_prep);
	sqlite3_finalize(insert_full_prep);	
	sqlite3_finalize(update_hash_prep);	
	sqlite3_finalize(select_prep);	
	
	return 0;
}