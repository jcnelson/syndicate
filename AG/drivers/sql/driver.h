#ifndef _SQL_DRIVER_H_
#define _SQL_DRIVER_H_

#include <map>
#include <string>
#include <set>
#include <sstream>

#include "libgateway.h"
#include "libsyndicate.h"
#include "map-parser.h"
#include "odbc-handler.h"
#include <sstream>

#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

using namespace std;

#define GATEWAY_REQUEST_TYPE_NONE       0
#define GATEWAY_REQUEST_TYPE_LOCAL_FILE 1
#define GATEWAY_REQUEST_TYPE_MANIFEST   2
#define SYNDICATEFS_AG_DB_PROTO         "synadb://"
#define SYNDICATEFS_AG_DB_DIR           1
#define SYNDICATEFS_AG_DB_FILE          2
#define FILE_PERMISSIONS_MASK		(S_IRUSR | S_IRGRP | S_IROTH )
#define DIR_PERMISSIONS_MASK		(S_IRUSR | S_IWUSR | S_IXUSR |\
       	S_IRGRP | S_IXGRP |\
	S_IXOTH)

#define GET_SYNADB_PATH(url)\
    (char*)url + strlen(SYNDICATEFS_AG_DB_PROTO)

struct gateway_ctx {
    int request_type;
    // file info 
    char const* file_path;
    // data buffer (manifest or remote block data)
    char* data;
    size_t data_len;
    size_t data_offset;
    off_t num_read;
    // file block info
    uint64_t block_id;
    // SQL query
    char* sql_query;
    // ODBC handle
    ODBCHandler& odh; 
    // is this corresponds to .db_info file?
    bool is_db_info;
    // are we done?
    bool complete;
};

struct path_comp {
    bool operator()(char *path1, char *path2) 
    {
	int p1_nr_comps = 0;
	int p2_nr_comps = 0;
	if (strcmp(path1, path2) == 0)
	    return false;
	size_t p1_len = strlen(path1);
	size_t p2_len = strlen(path2);
	size_t i;
	for (i=0; i<p1_len; i++) {
	    if (path1[i] == '/')
		p1_nr_comps++;
	}
	for (i=0; i<p2_len; i++) {
	    if (path2[i] == '/')
		p2_nr_comps++;
	}
	if (p1_nr_comps == p2_nr_comps) 
	    return true;
	else 
	    return p1_nr_comps < p2_nr_comps;
	
    }
};   

typedef map<string, struct md_entry*> content_map;
typedef map<string, struct map_info> query_map;
static int publish(const char *fpath, int type, struct map_info mi);

#endif //_SQL_DRIVER_H_

