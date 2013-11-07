/*
   Copyright 2013 The Trustees of Princeton University
   All Rights Reserved
*/

#ifndef _TRUNC_H_
#define _TRUNC_H_

#include "fs_entry.h"

int fs_entry_truncate( struct fs_core* core, char const* path, off_t size, uint64_t user, uint64_t volume );

int fs_entry_versioned_truncate(struct fs_core* core, const char* path, uint64_t file_id, uint64_t coordinator_id, off_t newsize,
                                int64_t known_version, uint64_t user, uint64_t volume, uint64_t gateway_id, bool check_file_id_and_coordinator_id );

int fs_entry_ftruncate( struct fs_core* core, struct fs_file_handle* fh, off_t size, uint64_t user, uint64_t volume );

#endif
