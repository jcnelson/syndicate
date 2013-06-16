/*
   Copyright 2013 The Trustees of Princeton University
   All Rights Reserved
*/


#ifndef _UNLINK_H_
#define _UNLINK_H_

#include "fs_entry.h"

int fs_entry_detach( struct fs_core* core, char const* path, uid_t user, gid_t vol );
int fs_entry_versioned_unlink( struct fs_core* core, char const* path, int64_t known_version, uid_t owner, gid_t volume );
int fs_entry_detach_lowlevel( struct fs_core* core, struct fs_entry* parent, struct fs_entry* child, bool remove_data );

#endif