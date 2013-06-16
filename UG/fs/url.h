/*
   Copyright 2013 The Trustees of Princeton University
   All Rights Reserved
*/


#ifndef _URL_H_
#define _URL_H_

#include "fs_entry.h"


// URL-fu
char* fs_entry_generic_url( char const* content_url, int64_t file_version, uint64_t block_id, int64_t block_version );
char* fs_entry_remote_block_url( char const* content_url, char const* fs_path, int64_t file_version, uint64_t block_id, int64_t block_version );
char* fs_entry_local_block_path( char const* data_root, char const* fs_path, int64_t file_version, uint64_t block_id, int64_t block_version );
char* fs_entry_local_block_url( struct fs_core* core, char const* fs_path, int64_t file_version, uint64_t block_id, int64_t block_version );
char* fs_entry_local_file_url( struct fs_core* core, char const* fs_path );
char* fs_entry_local_file_url( struct fs_core* core, char const* fs_path, int64_t version );
char* fs_entry_get_block_url( struct fs_core* core, struct fs_file_handle* fh, off_t offset );
char* fs_entry_get_block_url( struct fs_core* core, char const* fs_path, uint64_t block_id );
char* fs_entry_get_block_url( struct fs_core* core, char const* fs_path, struct fs_entry* fent, off_t offset );
char* fs_entry_public_file_url( char const* content_url, char const* fs_path, int64_t version );
char* fs_entry_public_file_url( struct fs_core* core, char const* fs_path, int64_t version );
char* fs_entry_public_file_url( char const* content_url, char const* fs_path );
char* fs_entry_public_file_url( struct fs_core* core, char const* fs_path );
char* fs_entry_public_dir_url( struct fs_core* core, char const* fs_path );
char* fs_entry_dir_path_from_public_url( struct fs_core* core, char const* dir_url );
char* fs_entry_local_staging_file_url( struct fs_core* core, char const* fs_path, int64_t file_version );
char* fs_entry_local_staging_file_url( struct fs_core* core, char const* fs_path );
char* fs_entry_staging_block_path( struct fs_core* core, char const* fs_path, int64_t file_version, uint64_t block_id, int64_t block_version );
char* fs_entry_local_staging_block_url( struct fs_core* core, char const* fs_path, int64_t file_version, uint64_t block_id, int64_t block_version );
char* fs_entry_public_staging_block_url( char const* host_url, char const* fs_path, int64_t file_version, uint64_t block_id, int64_t block_version );
char* fs_entry_public_block_url( struct fs_core* core, char const* fs_path, int64_t file_version, uint64_t block_id, int64_t block_version );
char* fs_entry_public_manifest_url( struct fs_core* core, char const* fs_path, int64_t version, struct timespec* ts );
char* fs_entry_remote_manifest_url( char const* fs_path, char const* file_url, int64_t version, struct timespec* ts );
char* fs_entry_manifest_path( char const* fs_path, char const* file_url, int64_t version, uint64_t time_sec, uint32_t time_nsec );
char* fs_entry_local_to_public( struct fs_core* core, char const* file_url, int64_t file_version );
char* fs_entry_local_to_public( struct fs_core* core, char const* file_url );
char* fs_entry_mkpath( char const* fs_path, int64_t version );
char* fs_entry_url_path( struct fs_file_handle* fh );
char* fs_entry_url_path( char const* fs_path, int64_t version );
char* fs_entry_replica_block_url( char const* url, int64_t version, uint64_t block_id, int64_t block_version );
char* fs_entry_replica_manifest_url( char const* url, int64_t version, struct timespec* ts );

#endif 