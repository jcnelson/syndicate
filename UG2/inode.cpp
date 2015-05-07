/*
   Copyright 2015 The Trustees of Princeton University

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

#include "inode.h"
#include "block.h"

// initialize common inode data 
// type should be MD_ENTRY_FILE or MD_ENTRY_DIR
// return 0 on success 
// return -ENOMEM on OOM 
static int UG_inode_init_common( struct UG_inode* inode, int type ) {
   
   memset( inode, 0, sizeof(struct UG_inode) );
   
   // regular file?
   if( type == MD_ENTRY_FILE ) {
         
      // sync queue 
      inode->sync_queue = SG_safe_new( UG_inode_fsync_queue_t() );
      if( inode->sync_queue == NULL ) {
         
         return -ENOMEM;
      }
      
      // dirty blocks 
      inode->dirty_blocks = SG_safe_new( UG_dirty_block_map_t() );
      if( inode->dirty_blocks == NULL ) {
         
         SG_safe_delete( inode->sync_queue );
         return -ENOMEM;
      }
   }
   
   return 0;
}

// initialize an inode, from an entry and basic data
// entry must be write-locked
// return 0 on success 
// return -ENOMEM on OOM 
int UG_inode_init( struct UG_inode* inode, struct fskit_entry* entry, uint64_t volume_id, uint64_t coordinator_id, int64_t file_version ) {
   
   int rc = 0;
   
   rc = UG_inode_init_common( inode, fskit_entry_get_type( entry ) == FSKIT_ENTRY_TYPE_FILE ? MD_ENTRY_FILE : MD_ENTRY_DIR );
   if( rc != 0 ) {
      
      return rc;
   }
   
   // manifest 
   rc = SG_manifest_init( &inode->manifest, volume_id, coordinator_id, entry->file_id, file_version );
   if( rc != 0 ) {
      
      SG_safe_delete( inode->sync_queue );
      SG_safe_delete( inode->dirty_blocks );
      return rc;
   }
   
   return 0;
}
   

// initialize an inode from an fskit_entry, and protobof'ed msent and mmsg
// return 0 on success 
// return -ENOMEM on OOM 
// return -EINVAL if the file IDs don't match 
int UG_inode_init_from_protobuf( struct UG_inode* inode, struct fskit_entry* entry, ms::ms_entry* msent, SG_messages::Manifest* mmsg ) {
   
   int rc = 0;
   
   // sanity check 
   if( entry->file_id != msent->file_id() ) {
      return -EINVAL;
   }
   
   rc = UG_inode_init_common( inode, fskit_entry_get_type( entry ) == FSKIT_ENTRY_TYPE_FILE ? MD_ENTRY_FILE : MD_ENTRY_DIR );
   if( rc != 0 ) {
      
      return rc;
   }
   
   // manifest 
   rc = SG_manifest_load_from_protobuf( &inode->manifest, mmsg );
   if( rc != 0 ) {
      
      SG_safe_delete( inode->sync_queue );
      SG_safe_delete( inode->dirty_blocks );
      return rc;
   }
   
   // fill in the rest 
   SG_manifest_set_modtime( &inode->manifest, msent->manifest_mtime_sec(), msent->manifest_mtime_nsec() );
   
   inode->write_nonce = msent->write_nonce();
   inode->xattr_nonce = msent->xattr_nonce();
   inode->generation = msent->generation();
   inode->max_read_freshness = msent->max_read_freshness();
   inode->max_write_freshness = msent->max_write_freshness();
   inode->ms_num_children = msent->num_children();
   inode->ms_capacity = msent->capacity();
   
   return 0;
}


// initialize an inode from an exported inode data, a manifest, and an fskit_entry 
// the inode will take ownership of the manifest; the caller should not do anything further with it.
// NOTE: file ID in inode_data and fent must match, as must their types
// return 0 on success 
// return -ENOMEM on OOM 
// return -EINVAL if the data is invalid (i.e. file IDs don't match, and such)
int UG_inode_init_from_export( struct UG_inode* inode, struct md_entry* inode_data, struct SG_manifest* manifest, struct fskit_entry* fent ) {
   
   int rc = 0;
   struct stat sb;
   int type = fskit_entry_get_type( fent );
   
   fskit_entry_fstat( fent, &sb );
   
   // ID sanity check 
   if( inode_data->file_id != sb.st_ino ) {
      return -EINVAL;
   }
   
   // type sanity check 
   if( type == FSKIT_ENTRY_TYPE_FILE && inode_data->type != MD_ENTRY_FILE ) {
      return -EINVAL;
   }
   
   if( type == FSKIT_ENTRY_TYPE_DIR && inode_data->type != MD_ENTRY_DIR ) {
      return -EINVAL;
   }
      
   rc = UG_inode_init_common( inode, fskit_entry_get_type( fent ) );
   if( rc != 0 ) {
      
      return rc;
   }
   
   inode->manifest = *manifest;
   
   SG_manifest_set_modtime( &inode->manifest, inode_data->manifest_mtime_sec, inode_data->manifest_mtime_nsec );
   
   inode->write_nonce = inode_data->write_nonce;
   inode->xattr_nonce = inode_data->xattr_nonce;
   inode->generation = inode_data->generation;
   inode->max_read_freshness = inode_data->max_read_freshness;
   inode->max_write_freshness = inode_data->max_write_freshness;
   inode->ms_num_children = inode_data->num_children;
   inode->ms_capacity = inode_data->capacity;
   
   return 0;
}


// free an inode
// NOTE: destroys its dirty blocks
// always succeeds
int UG_inode_free( struct UG_inode* inode ) {
   
   SG_safe_delete( inode->sync_queue );
   SG_safe_delete( inode->dirty_blocks );
   SG_manifest_free( &inode->manifest );
   
   memset( inode, 0, sizeof(struct UG_inode) );
   
   return 0;
}


// set up a file handle
// NOTE: inode->entry must be read-locked
// return 0 on success
// return -EINVAL if the inode is malformed (NOTE: indicates a bug!)
// return -ENOMEM on OOM 
int UG_file_handle_init( struct UG_file_handle* fh, struct UG_inode* inode, int flags ) {
   
   if( inode->entry == NULL ) {
      return -EINVAL;
   }
   
   fh->evicts = SG_safe_new( UG_inode_block_eviction_map_t() );
   if( fh->evicts == NULL ) {
      return -ENOMEM;
   }
   
   fh->inode_ref = inode;
   fh->flags = flags;
   
   return 0;
}


// free a file handle 
// return 0 on success 
// return -ENOMEM on OOM 
int UG_file_handle_free( struct UG_file_handle* fh ) {
   
   SG_safe_delete( fh->evicts );
   
   memset( fh, 0, sizeof(struct UG_file_handle) );
   
   return 0;
}


// export an inode to an md_entry 
// return 0 on success
// return -ENOMEM on OOM 
// NOTE: src->entry must be read-locked
int UG_inode_export( struct md_entry* dest, struct UG_inode* src, uint64_t parent_id, char const* parent_name ) {
   
   // get type
   int type = fskit_entry_get_type( src->entry );
   
   if( type == FSKIT_ENTRY_TYPE_FILE ) {
      dest->type = MD_ENTRY_FILE;
   }
   else if( type == FSKIT_ENTRY_TYPE_DIR ) {
      dest->type = MD_ENTRY_DIR;
   }
   else {
      // invalid 
      return -EINVAL;
   }
   
   char* name = fskit_entry_get_name( src->entry );
   if( name == NULL ) {
      
      return -ENOMEM;
   }
   
   char* parent_name_dup = SG_strdup_or_null( parent_name );
   if( parent_name_dup == NULL && parent_name != NULL ) {
      
      SG_safe_free( name );
      return -ENOMEM;
   }
   
   dest->type = type;
   dest->name = name;
   dest->file_id = fskit_entry_get_file_id( src->entry );
   
   fskit_entry_get_ctime( src->entry, &dest->ctime_sec, &dest->ctime_nsec );
   fskit_entry_get_mtime( src->entry, &dest->mtime_sec, &dest->mtime_nsec );
   
   SG_manifest_get_modtime( &src->manifest, &dest->manifest_mtime_sec, &dest->manifest_mtime_nsec );
   
   dest->write_nonce = src->write_nonce;
   dest->xattr_nonce = src->xattr_nonce;
   dest->version = SG_manifest_get_file_version( &src->manifest );
   dest->max_read_freshness = src->max_read_freshness;
   dest->max_write_freshness = src->max_write_freshness;
   dest->owner = fskit_entry_get_owner( src->entry );
   dest->coordinator = SG_manifest_get_coordinator( &src->manifest );
   dest->volume = SG_manifest_get_volume_id( &src->manifest );
   dest->mode = fskit_entry_get_mode( src->entry );
   dest->size = fskit_entry_get_size( src->entry );
   dest->error = 0;
   dest->generation = src->generation;
   dest->num_children = src->ms_num_children;
   dest->capacity = src->ms_capacity;
   dest->parent_id = parent_id;
   dest->parent_name = parent_name_dup;
   
   return 0;
}

// does an exported inode's type match the inode's type?
// return 1 if so
// return 0 if not 
// NOTE: dest->entry must be read-locked
int UG_inode_export_match_type( struct UG_inode* dest, struct md_entry* src ) {
   
   int type = fskit_entry_get_type( dest->entry );
   bool valid_type = false;
   
   
   if( type == FSKIT_ENTRY_TYPE_FILE && src->type == MD_ENTRY_FILE ) {
      
      valid_type = true;
   }
   else if( type == FSKIT_ENTRY_TYPE_DIR && src->type == MD_ENTRY_DIR ) {
      
      valid_type = true;
   }
   
   if( !valid_type ) {
      return 0;
   }
   else {
      return 1;
   }
}


// does an exported inode's size match the inode's size?
// return 1 if so, 0 if not 
// NOTE: dest->entry must be read-locked
int UG_inode_export_match_size( struct UG_inode* dest, struct md_entry* src ) {
   
   struct stat sb;
   
   fskit_entry_fstat( dest->entry, &sb );
   
   // size matches?
   if( fskit_entry_get_size( UG_inode_fskit_entry( *dest ) ) != src->size ) {
      
      return 0;
   }
   else {
      
      return 1;
   }
}


// does an exported inode's version match an inode's version?
// return 1 if so, 0 if not 
// NOTE: dest->entry must be read-locked
int UG_inode_export_match_version( struct UG_inode* dest, struct md_entry* src ) {
   
   // version matches?
   if( SG_manifest_get_file_version( &dest->manifest ) != src->version ) {
      
      return 0;
   }
   else {
      
      return 1;
   }
}


// does an exported inode's file ID match an inode's file ID?
// return 1 if so, 0 if not 
// NOTE: dest->entry must be read-locked 
int UG_inode_export_match_file_id( struct UG_inode* dest, struct md_entry* src ) {
   
   // file ID matches?
   if( fskit_entry_get_file_id( dest->entry ) != src->file_id ) {
      
      return 0;
   }
   else {
      
      return 1;
   }
}


// does an exported inode's name match the inode's name?
// return 1 if so, 0 if not
// return -ENOMEM on OOM
// NOTE: dest->entry must be read-locked 
int UG_inode_export_match_name( struct UG_inode* dest, struct md_entry* src ) {
   
   // name matches?
   char* name = fskit_entry_get_name( dest->entry );
   if( name == NULL ) {
      
      return -ENOMEM;
   }
   
   if( strcmp( name, src->name ) != 0 ) {
      
      SG_safe_free( name );
      return 0;
   }
   else {
      
      SG_safe_free( name );
      return 1;
   }
}


// import inode metadata from an md_entry
// inode must already be initialized 
// NOTE: dest's type, file ID, version, name, and size must match src's.  The caller must make sure of this out-of-band,
// since changing these requires some kind of I/O or directory structure clean-up
// return 0 on success 
// return -EINVAL if the types, IDs, versions, sizes, or names don't match
// NOTE: src->entry must be write-locked
int UG_inode_import( struct UG_inode* dest, struct md_entry* src ) {
   
   if( !UG_inode_export_match_file_id( dest, src ) ) {
      
      return -EINVAL;
   }
   
   if( UG_inode_export_match_name( dest, src ) <= 0 ) {
      
      return -EINVAL;
   }
   
   if( !UG_inode_export_match_size( dest, src ) ) {
      
      return -EINVAL;
   }
   
   if( !UG_inode_export_match_type( dest, src ) ) {
      
      return -EINVAL;
   }
   
   if( !UG_inode_export_match_version( dest, src ) ) {
      
      return -EINVAL;
   }
   
   struct timespec ts;
   
   // looks good!
   ts.tv_sec = src->ctime_sec;
   ts.tv_nsec = src->ctime_nsec;
   fskit_entry_set_ctime( dest->entry, &ts );
   
   ts.tv_sec = src->mtime_sec;
   ts.tv_nsec = src->mtime_nsec;
   fskit_entry_set_mtime( dest->entry, &ts );
   
   dest->ms_write_nonce = src->write_nonce;
   dest->ms_xattr_nonce = src->xattr_nonce;
   
   SG_manifest_set_coordinator_id( &dest->manifest, src->coordinator );
   SG_manifest_set_owner_id( &dest->manifest, src->owner );
   
   dest->max_read_freshness = src->max_read_freshness;
   dest->max_write_freshness = src->max_write_freshness;
   
   fskit_entry_set_owner_and_group( dest->entry, src->owner, src->volume );
   fskit_entry_set_mode( dest->entry, src->mode );
   
   dest->generation = src->generation;
   dest->ms_num_children = src->num_children;
   dest->ms_capacity = src->capacity;
   
   return 0;
}


// does an inode's manifest have a more recent modtime than the given one?
// return true if so; false if not 
bool UG_inode_manifest_is_newer_than( struct SG_manifest* manifest, int64_t mtime_sec, int32_t mtime_nsec ) {
   
   struct timespec old_manifest_ts;
   struct timespec new_manifest_ts;
   
   new_manifest_ts.tv_sec = mtime_sec;
   new_manifest_ts.tv_nsec = mtime_nsec;
   
   old_manifest_ts.tv_sec = SG_manifest_get_modtime_sec( manifest );
   old_manifest_ts.tv_nsec = SG_manifest_get_modtime_nsec( manifest );
   
   bool newer = (md_timespec_diff_ms( &new_manifest_ts, &old_manifest_ts ) > 0);
   
   return newer;
}

// merge new manifest block data into an inode's manifest (i.e. from reloading it remotely, or handling a remote write).
// evict now-stale cached data and overwritten dirty blocks.
// remove now-invalid garbage block data.
// return 0 on success, and populate the inode's manifest with the given manifest's block data
// return -ENOMEM on OOM
// NOTE: inode->entry must be write-locked 
// NOTE: this method is idempotent, and will partially-succeed if it returns -ENOMEM.  Callers are encouraged to try and retry until it succeeds
// NOTE: this method is a commutative and associative on manifests--given manifests A, B, and C, doesn't matter what order they get merged
// NOTE: (i.e. merge( merge(A, B), C ) === merge( A, merge( B, C ) ) and merge( A, B ) == merge( B, A ))
// NOTE: does *NOT* merge size, does *NOT* merge modtime, and does *NOT* attempt to truncate
int UG_inode_manifest_merge_blocks( struct SG_gateway* gateway, struct UG_inode* inode, struct SG_manifest* new_manifest ) {
   
   int rc = 0;
   struct md_syndicate_cache* cache = SG_gateway_cache( gateway );
   
   bool replace;        // set to true if we will replace blocks in the manifest; false if not (i.e. depends on whether the "new_manifest" is actually newer)
   
   // i.e. if our manifest is newer than the "new" manifest, then don't replace blocks on conflict
   replace = !UG_inode_manifest_is_newer_than( UG_inode_manifest( *inode ), SG_manifest_get_modtime_sec( new_manifest ), SG_manifest_get_modtime_nsec( new_manifest ) );
   
   // add all blocks in new_manifest
   for( SG_manifest_block_iterator itr = SG_manifest_block_iterator_begin( new_manifest ); itr != SG_manifest_block_iterator_end( new_manifest ); itr++ ) {
      
      uint64_t block_id = SG_manifest_block_iterator_id( itr );
      struct SG_manifest_block* new_block = SG_manifest_block_iterator_block( itr );
      
      UG_dirty_block_map_t::iterator dirty_block_itr;
      bool have_dirty_block = false;
      struct UG_dirty_block* dirty_block = NULL;
      
      struct SG_manifest_block* replaced_block = NULL;
      
      struct SG_manifest_block* existing_block = SG_manifest_block_lookup( UG_inode_manifest( *inode ), block_id );
      int64_t existing_block_version = 0;
      
      if( existing_block == NULL ) {
         
         // nothing to worry about
         continue;
      }
      
      if( SG_manifest_block_version( existing_block ) == SG_manifest_block_version( new_block ) ) {
         
         // already merged, or no change
         continue;
      }
      
      // if the local block is dirty, keep the local block 
      if( SG_manifest_block_is_dirty( existing_block ) ) {
         
         continue;
      }
      
      // preserve existing block data that will get overwritten 
      existing_block_version = SG_manifest_block_version( existing_block );
      
      // merge into current manifest, replacing the old one *if* the new_manifest is actually newer (makes this method commutative, associative)
      // that is, only overwrite a block if the block is not dirty, and if the new_manifest has a newer modification time (this in turn is guaranteed
      // to be monotonically increasing since there is at most one coordinator).
      rc = SG_manifest_put_block( UG_inode_manifest( *inode ), new_block, replace );
      if( rc != 0 ) {
         
         break;
      }
      
      // clear cached block (idempotent)
      md_cache_evict_block( cache, UG_inode_file_id( *inode ), UG_inode_file_version( *inode ), block_id, existing_block_version );
      
      // clear dirty block (idempotent)
      dirty_block_itr = UG_inode_dirty_blocks( *inode )->find( block_id );
      if( dirty_block_itr != UG_inode_dirty_blocks( *inode )->end() ) {
         
         dirty_block = &dirty_block_itr->second;
         UG_dirty_block_evict_and_free( cache, inode, dirty_block );
         
         have_dirty_block = true;
      }
      
      // clear invalidated garbage, if there is any (idempotent)
      if( have_dirty_block ) {
         
         replaced_block = SG_manifest_block_lookup( UG_inode_replaced_blocks( *inode ), block_id );
         if( replaced_block != NULL ) {
            
            SG_manifest_delete_block( UG_inode_replaced_blocks( *inode ), block_id );
         }
      }
   }
   
   return rc;
}


// trim an inode's dirty blocks
// flush all blocks but those id'ed in *preserve
// ensure that all blocks in *preserve are unshared 
// return 0 on success
// return -ENOMEM on OOM 
// return -errno on failure to flush 
// NOTE: inode->entry should be write-locked--this method is not reentrant or thread-safe
// if this method fails, it can be safely tried again.
int UG_inode_dirty_blocks_trim( struct SG_gateway* gateway, char const* fs_path, struct UG_inode* inode, uint64_t* preserve, size_t preserve_len ) {
   
   int rc = 0;
   int worst_rc = 0;
   
   for( UG_dirty_block_map_t::iterator itr = inode->dirty_blocks->begin(); itr != inode->dirty_blocks->end(); itr++ ) {
      
      bool do_preserve = false;
      
      // preserve?
      if( preserve != NULL ) {
         
         for( size_t i = 0; i < preserve_len; i++ ) {
          
            if( itr->first == preserve[i] ) {
               
               // don't flush this one, but make sure it's unshared 
               if( !UG_dirty_block_unshared( itr->second ) && UG_dirty_block_buf( itr->second ).data != NULL ) {
                  
                  rc = UG_dirty_block_buf_unshare( &itr->second );
                  if( rc != 0 ) {
                     
                     // probably OOM 
                     SG_error("UG_dirty_block_buf_unshare( %" PRIu64 ".%" PRId64 " ) rc = %d\n", UG_dirty_block_id( itr->second ), UG_dirty_block_version( itr->second ), rc );
                     return rc;
                  }
                  
                  do_preserve = true;
               }
            }
         }
      }
      
      if( !do_preserve && !UG_dirty_block_is_flushing( itr->second ) && UG_dirty_block_fd( itr->second ) < 0 ) {
         
         // flush out of RAM
         rc = UG_dirty_block_flush_async( gateway, fs_path, UG_inode_file_id( *inode ), UG_inode_file_version( *inode ), &itr->second );
         if( rc != 0 ) {
            
            SG_error("UG_dirty_block_flush_async( %" PRIX64 ".%" PRId64 "[%" PRIu64 ".%" PRId64 "] ) rc = %d\n", 
                     UG_inode_file_id( *inode ), UG_inode_file_version( *inode ), UG_dirty_block_id( itr->second ), UG_dirty_block_version( itr->second ), rc );
            
            worst_rc = rc;
            break;
         }
      }
   }
   
   // finish flushing all blocks
   // try them all even if we error
   for( UG_dirty_block_map_t::iterator itr = inode->dirty_blocks->begin(); itr != inode->dirty_blocks->end(); itr++ ) {
      
      if( UG_dirty_block_is_flushing( itr->second ) ) {
         
         rc = UG_dirty_block_flush_finish( &itr->second );
         if( rc != 0 ) {
            
            SG_error("UG_dirty_block_flush_finish( %" PRIX64 ".%" PRId64 "[%" PRIu64 ".%" PRId64 "] ) rc = %d\n", 
                     UG_inode_file_id( *inode ), UG_inode_file_version( *inode ), UG_dirty_block_id( itr->second ), UG_dirty_block_version( itr->second ), rc );
            
            worst_rc = rc;
         }
      }
   }
   
   return worst_rc;
}


// extract the modified dirty blocks from the inode 
// return 0 on success, and fill in *modified. the *inode will no longer have modified dirty blocks
// return -ENOMEM on OOM 
// NOTE: inode->entry must be write-locked
int UG_inode_dirty_blocks_extract_modified( struct UG_inode* inode, UG_dirty_block_map_t* modified ) {
   
   if( modified != NULL ) {
      
      for( UG_dirty_block_map_t::iterator itr = UG_inode_dirty_blocks( *inode )->begin(); itr != UG_inode_dirty_blocks( *inode )->end(); itr++ ) {
         
         // dirty?
         if( !UG_dirty_block_dirty( itr->second ) ) {
            
            itr++;
            continue;
         }
         
         try {
            
            (*modified)[ itr->first ] = itr->second;
         }
         catch( bad_alloc& ba ) {
            return -ENOMEM;
         }
      }
   }
   
   // and remove from the inode 
   for( UG_dirty_block_map_t::iterator itr = modified->begin(); itr != modified->end(); itr++ ) {
      
      UG_inode_dirty_blocks( *inode )->erase( itr->first );
   }
   
   return 0;
}


// return extracted dirty blocks to an inode.  clear them out of extracted
// return 0 on success
// return -ENOMEM on OOM 
// NOTE: inode->entry must be write-locked; locked in the same context as the _extract_ method was called 
// NOTE: this method is idempotent.  call it multiple times if it fails
int UG_inode_dirty_blocks_return( struct UG_inode* inode, UG_dirty_block_map_t* extracted ) {
   
   int rc = 0;
   
   for( UG_dirty_block_map_t::iterator itr = extracted->begin(); itr != extracted->end(); ) {
      
      try { 
         
         (*inode->dirty_blocks)[ itr->first ] = itr->second;
      }
      catch( bad_alloc& ba ) {
         
         rc = -ENOMEM;
         break;
      }
      
      UG_dirty_block_map_t::iterator old_itr = itr;
      itr++;
      
      extracted->erase( old_itr );
   }
   
   return rc;
}


// clear replaced block data for dirty blocks, since we've extracted them already
// return 0 on success
int UG_inode_replaced_blocks_clear( struct UG_inode* inode, UG_dirty_block_map_t* dirty_blocks ) {
   
   // clear out replaced block info--we're vacuuming them now.
   for( UG_dirty_block_map_t::iterator itr = dirty_blocks->begin(); itr != dirty_blocks->end(); itr++ ) {
      
      SG_manifest_delete_block( UG_inode_replaced_blocks( *inode ), itr->first );
   }
   
   return 0;
}

// cache a non-dirty block to an inode's dirty-block set.
// fails if there is already a block cached with a different version.
// succeeds if there is already a block cached, but with the same version.
// does not affect the inode's manifest or replaced_block sets.
// return 0 on success
// return -ENOMEM on OOM 
// return -EINVAL if the block is dirty
// return -EEXIST if the block would replace a different block
// NOTE: inode->entry must be write-locked
// NOTE: inode takes ownership of dirty_block's contents.
int UG_inode_dirty_block_cache( struct UG_inode* inode, struct UG_dirty_block* dirty_block ) {
   
   // dirty? invalid 
   if( UG_dirty_block_dirty( *dirty_block ) ) {
      
      return -EINVAL;
   }
   
   struct UG_dirty_block* old_dirty_block = NULL;
   UG_dirty_block_map_t::iterator old_dirty_block_itr;
   
   // already cached?
   old_dirty_block_itr = UG_inode_dirty_blocks( *inode )->find( UG_dirty_block_id( *dirty_block ) );
   if( old_dirty_block_itr != UG_inode_dirty_blocks( *inode )->end() ) {
      
      old_dirty_block = &old_dirty_block_itr->second;
      
      // there's a block here. is it the same one?
      if( UG_dirty_block_version( *dirty_block ) == UG_dirty_block_version( *old_dirty_block ) ) {
         
         return 0;
      }
      else {
         
         return -EEXIST;
      }
   }
   
   // cache 
   try {
      
      (*inode->dirty_blocks)[ UG_dirty_block_id( *dirty_block ) ] = *dirty_block;
   }
   catch( bad_alloc& ba ) {
      
      return -ENOMEM;
   }
   
   return 0;
}


// commit a single dirty block to an inode, optionally replacing an older version of the block.
// update the inode's manifest (putting a dirty block info), and remember block information for blocks that must be garbage-collected.
// evict the old version of the block, if it is cached.
// return 0 on success.  The inode takes ownership of dirty_block.
// return -ENOMEM on OOM 
// return -EINVAL if dirty_block is not dirty
// NOTE: inode->entry must be write-locked!
// NOTE: inode takes ownership of dirty_block's contents
int UG_inode_dirty_block_commit( struct SG_gateway* gateway, struct UG_inode* inode, struct UG_dirty_block* dirty_block ) {
   
   int rc = 0;
   
   // not dirty? do nothing 
   if( !UG_dirty_block_dirty( *dirty_block ) ) {
      
      return -EINVAL;
   }
   
   struct md_syndicate_cache* cache = SG_gateway_cache( gateway );
   
   struct SG_manifest_block old_block_info;
   struct UG_dirty_block* old_dirty_block = NULL;
   
   // what blocks are being replaced?
   struct SG_manifest_block* old_block_info_ref = SG_manifest_block_lookup( UG_inode_manifest( *inode ), UG_dirty_block_id( *dirty_block ) );
   struct SG_manifest_block* old_replaced_block_info = SG_manifest_block_lookup( UG_inode_replaced_blocks( *inode ), UG_dirty_block_id( *dirty_block ) );
   
   // store a copy, so we can put it back on failure 
   if( old_block_info_ref != NULL ) {
      
      rc = SG_manifest_block_dup( &old_block_info, old_block_info_ref );
      if( rc != 0 ) {
         
         // OOM 
         return rc;
      }
   }
   
   // find the old dirty block
   UG_dirty_block_map_t::iterator old_dirty_block_itr = UG_inode_dirty_blocks( *inode )->find( UG_dirty_block_id( *dirty_block ) );
   if( old_dirty_block_itr != UG_inode_dirty_blocks( *inode )->end() ) {
      
      old_dirty_block = &old_dirty_block_itr->second;
   }
   
   // update the manifest 
   rc = SG_manifest_put_block( UG_inode_manifest( *inode ), UG_dirty_block_info( *dirty_block ), true );
   if( rc != 0 ) {
      
      SG_error("SG_manifest_put_block( %" PRIX64 ".%" PRIu64" [%" PRId64 ".%" PRIu64 "] ) rc = %d\n", 
               UG_inode_file_id( *inode ), UG_inode_file_version( *inode ), UG_dirty_block_id( *dirty_block ), UG_dirty_block_version( *dirty_block ), rc );
      
      
      if( old_block_info_ref != NULL ) {
         
         SG_manifest_block_free( &old_block_info );
      }
      
      return rc;
   }
   
   // insert the new dirty block 
   try {
      
      (*inode->dirty_blocks)[ UG_dirty_block_id( *dirty_block ) ] = *dirty_block;
   }
   catch( bad_alloc& ba ) {
      
      rc = -ENOMEM;
      
      // put the old block data back 
      // NOTE: guaranteed to succeed, since we're replacing without copying or allocating 
      SG_manifest_put_block_nocopy( UG_inode_manifest( *inode ), &old_block_info, true );
      
      goto UG_inode_block_commit_out;
   }
   
   if( old_block_info_ref != NULL ) {
      
      if( old_replaced_block_info == NULL ) {
         
         // this block is to be replaced, but we'll need to garbage-collect the old version 
         rc = SG_manifest_put_block_nocopy( UG_inode_replaced_blocks( *inode ), &old_block_info, true );
         if( rc != 0 ) {
            
            SG_error("SG_manifest_put_block( %" PRIX64 ".%" PRIu64" [%" PRId64 ".%" PRIu64 "] ) rc = %d\n", 
                     UG_inode_file_id( *inode ), UG_inode_file_version( *inode ), old_block_info.block_id, old_block_info.block_version, rc );
            
            // put the old block data back 
            // NOTE: guaranteed to succeed, since we're replacing without copying or allocating 
            SG_manifest_put_block_nocopy( UG_inode_manifest( *inode ), &old_block_info, true );
            
            // NOTE: guaranteed to succeed, since we're replacing without copying or allocating 
            if( old_dirty_block != NULL ) {
               
               (*inode->dirty_blocks)[ UG_dirty_block_id( *dirty_block ) ] = *old_dirty_block;
            }
            
            goto UG_inode_block_commit_out;
         }
      }
   }
   
   // this block is dirty--keep it in the face of future manifest refreshes, until we replicate
   SG_manifest_set_block_dirty( UG_inode_manifest( *inode ), UG_dirty_block_id( *dirty_block ), true );
   
UG_inode_block_commit_out:
   
   if( rc == 0 ) {
      
      if( old_dirty_block != NULL ) {
      
         // clear out this block
         UG_dirty_block_evict_and_free( cache, inode, old_dirty_block );
      }
   }
   
   return rc;
}


// remember to evict a non-dirty block when we close this descriptor 
// return 0 on success
// return -ENOMEM on OOM 
int UG_file_handle_evict_add_hint( struct UG_file_handle* fh, uint64_t block_id, int64_t block_version ) {
   
   try {
      (*fh->evicts)[ block_id ] = block_version;
   }
   catch( bad_alloc& ba ) {
      return -ENOMEM;
   }
   
   return 0;
}


// clear all non-dirty blocks from the inode that this file handle created
// return 0 on success
// NOTE: fh->inode_ref->entry must be write-locked
int UG_file_handle_evict_blocks( struct UG_file_handle* fh ) {
   
   for( UG_inode_block_eviction_map_t::iterator itr = fh->evicts->begin(); itr != fh->evicts->end(); itr++ ) {
      
      UG_dirty_block_map_t::iterator block_itr = fh->inode_ref->dirty_blocks->find( itr->first );
      if( block_itr != fh->inode_ref->dirty_blocks->end() ) {
         
         struct UG_dirty_block* dirty_block = &block_itr->second;
         
         // clear, if the version matches and it's not dirty 
         if( UG_dirty_block_version( *dirty_block ) == itr->second && !UG_dirty_block_dirty( *dirty_block ) ) {
            
            // evict 
            fh->inode_ref->dirty_blocks->erase( block_itr );
            
            UG_dirty_block_free( dirty_block );
         }
      }
   }
   
   fh->evicts->clear();
   
   return 0;
}


// replace the manifest of an inode 
// free the old one.
// always succeeds
// NOTE: inode->entry must be write-locked 
int UG_inode_manifest_replace( struct UG_inode* inode, struct SG_manifest* manifest ) {
   
   struct SG_manifest old_manifest;
   
   old_manifest = inode->manifest;
   inode->manifest = *manifest;
   
   SG_manifest_free( &old_manifest );
   
   return 0;
}


// find all blocks in the inode that would be removed by a truncation
// return 0 on success, and populate *removed 
// return -ENOMEM on OOM 
// NOTE: inode->entry must be at least read-locked.
int UG_inode_truncate_find_removed( struct SG_gateway* gateway, struct UG_inode* inode, off_t new_size, struct SG_manifest* removed ) {
   
   int rc = 0;
   struct ms_client* ms = SG_gateway_ms( gateway );
   uint64_t block_size = ms_client_get_volume_blocksize( ms );
   
   uint64_t drop_block_id = (new_size / block_size);
   if( new_size % block_size != 0 ) {
      drop_block_id ++;
   }
   
   uint64_t max_block_id = SG_manifest_get_block_range( UG_inode_manifest( *inode ) );
      
   // copy over blocks to removed
   for( uint64_t dead_block_id = drop_block_id; dead_block_id <= max_block_id; dead_block_id++ ) {
      
      struct SG_manifest_block* block_info = SG_manifest_block_lookup( UG_inode_manifest( *inode ), dead_block_id );
      if( block_info == NULL ) {
         
         // write hole 
         continue;
      }
      
      if( removed != NULL ) {
         
         rc = SG_manifest_put_block( removed, block_info, true );
         if( rc != 0 ) {
            
            // OOM
            return -ENOMEM;
         }
      }
   }
   
   return 0;
}


// remove all blocks beyond a given size (if there are any), and set the inode to the new size
// drop cached blocks, drop dirty blocks, remove blocks from the manifest
// if removed is not NULL, populate it with removed blocks
// return 0 on success
// NOTE: inode->entry must be write-locked
// NOTE: if this method fails with -ENOMEM, and *removed is not NULL, the caller should free up *removed
// NOTE: if new_version is 0, the version will *not* be changed
int UG_inode_truncate( struct SG_gateway* gateway, struct UG_inode* inode, off_t new_size, int64_t new_version ) {
   
   struct ms_client* ms = SG_gateway_ms( gateway );
   uint64_t block_size = ms_client_get_volume_blocksize( ms );
   
   uint64_t drop_block_id = (new_size / block_size);
   if( new_size % block_size != 0 ) {
      drop_block_id ++;
   }
   
   uint64_t max_block_id = SG_manifest_get_block_range( UG_inode_manifest( *inode ) );
   
   int64_t old_version = UG_inode_file_version( *inode );
   
   struct md_syndicate_cache* cache = SG_gateway_cache( gateway );
   
   // go through the manifest and drop locally-cached blocks
   for( uint64_t dead_block_id = drop_block_id; dead_block_id <= max_block_id; dead_block_id++ ) {
      
      struct SG_manifest_block* block_info = SG_manifest_block_lookup( UG_inode_manifest( *inode ), dead_block_id );
      if( block_info == NULL ) {
         
         // write hole 
         continue;
      }
      
      // clear dirty block
      UG_dirty_block_map_t::iterator dirty_block_itr = UG_inode_dirty_blocks( *inode )->find( drop_block_id );
      if( dirty_block_itr != UG_inode_dirty_blocks( *inode )->end() ) {
         
         struct UG_dirty_block* dirty_block = &dirty_block_itr->second;
         
         UG_dirty_block_evict_and_free( cache, inode, dirty_block );
         UG_inode_dirty_blocks( *inode )->erase( dirty_block_itr );
      }
      
      // clear cached block 
      md_cache_evict_block( cache, UG_inode_file_id( *inode ), UG_inode_file_version( *inode ), dead_block_id, SG_manifest_block_version( block_info ) );
   }
   
   if( new_version != 0 ) {
         
      // next version 
      SG_manifest_set_file_version( UG_inode_manifest( *inode ), new_version );
      
      // reversion 
      md_cache_reversion_file( cache, UG_inode_file_id( *inode ), old_version, new_version );
   }
   
   // drop extra manifest blocks 
   SG_manifest_truncate( UG_inode_manifest( *inode ), (new_size / block_size) );
   
   // set new size 
   SG_manifest_set_size( UG_inode_manifest( *inode ), new_size );
   
   return 0;
}

// resolve a path to an inode and it's parent's information 
// return a pointer to the locked fskit_entry on success, and set *parent_id and *parent_name (*parent_name will be malloc'ed)
// return NULL on error, and set *rc to non-zero
static struct fskit_entry* UG_inode_resolve_path_and_parent( struct fskit_core* fs, char const* fs_path, bool writelock, int* rc, uint64_t* parent_id, char** parent_name ) {
   
   struct resolve_parent {
      
      uint64_t parent_id;
      char* parent_name;
      
      uint64_t file_id;
      char* file_name;
      
      // callback to fskit_entry_resolve_path_cls to remember the parent's name and ID
      // return 0 on success 
      // return -ENOMEM on OOM
      static int remember_parent( struct fskit_entry* cur, void* cls ) {
         
         struct resolve_parent* rp = (struct resolve_parent*)cls;
         
          rp->parent_id = rp->file_id;
          
          SG_safe_free( rp->parent_name );
          rp->parent_name = rp->file_name;
          
          rp->file_id = cur->file_id;
          rp->file_name = SG_strdup_or_null( cur->name );
          
          if( rp->file_name == NULL ) {
             
             SG_safe_free( rp->parent_name );
             return -ENOMEM;
          }
          
          return 0;
      }
   };
   
   struct resolve_parent rp;
   
   struct fskit_entry* fent = fskit_entry_resolve_path_cls( fs, fs_path, 0, 0, writelock, rc, resolve_parent::remember_parent, &rp );
   if( fent == NULL ) {
      
      return NULL;
   }
   
   *parent_id = rp.parent_id;
   *parent_name = rp.parent_name;
   
   return fent;
}


// export an fskit_entry inode from the filesystem
// return 0 on success, and fill in *inode_data and *renaming from the inode
// return -ENOMEM on OOM 
int UG_inode_export_fs( struct fskit_core* fs, char const* fs_path, struct md_entry* inode_data ) {
   
   int rc = 0;
   struct fskit_entry* fent = NULL;
   struct UG_inode* inode = NULL;
   
   uint64_t parent_id = 0;
   char* parent_name = NULL;
   
   fent = UG_inode_resolve_path_and_parent( fs, fs_path, false, &rc, &parent_id, &parent_name );
   if( fent == NULL ) {
      
      return rc;
   }
   
   inode = (struct UG_inode*)fskit_entry_get_user_data( fent );
   
   rc = UG_inode_export( inode_data, inode, parent_id, parent_name );
   SG_safe_free( parent_name );
   
   fskit_entry_unlock( fent );
   
   return rc;
}


// push a sync context to the sync queue
// return 0 on success
// return -ENOMEM on OOM 
int UG_inode_sync_queue_push( struct UG_inode* inode, struct UG_sync_context* sync_context ) {
   
   try {
      inode->sync_queue->push( sync_context );
      return 0;
   }
   catch( bad_alloc& ba ) {
      return -ENOMEM;
   }
}

// pop a sync context from the sync queue and return it 
// return NULL if empty 
struct UG_sync_context* UG_inode_sync_queue_pop( struct UG_inode* inode ) {
 
   struct UG_sync_context* ret = NULL;
   
   if( inode->sync_queue->size() > 0 ) {
      ret = inode->sync_queue->front();
      inode->sync_queue->pop();
   }
   
   return ret;
}

// clear the list of replaced blocks; e.g. on successful replication 
// always succeeds
int UG_inode_clear_replaced_blocks( struct UG_inode* inode ) {
   
   SG_manifest_clear( &inode->replaced_blocks );
   return 0;
}


// replace a UG's dirty blocks with a new caller-allocated dirty block map 
// always succeeds
UG_dirty_block_map_t* UG_inode_replace_dirty_blocks( struct UG_inode* inode, UG_dirty_block_map_t* new_dirty_blocks ) {
   
   UG_dirty_block_map_t* ret = inode->dirty_blocks;
   inode->dirty_blocks = new_dirty_blocks;
   return ret;
}