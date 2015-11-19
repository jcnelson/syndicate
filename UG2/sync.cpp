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

#include "sync.h"
#include "consistency.h"
#include "vacuumer.h"
#include "replication.h"
#include "write.h"
#include "inode.h"
#include "core.h"

// begin flushing a set of dirty blocks to disk, asynchronously.
// fails fast, in which case some (but not all) of the blocks in dirty_blocks are written.  The caller should call UG_write_blocks_wait() on failure, before cleaning up.
// However, this method is also idempotent--it can be called multiple times on the same dirty_blocks, and each block will flush to disk cache at most once.
// NOTE: each dirty block must be marked as dirty; otherwise it will not be processed.
// return 0 on success 
// return -ENOMEM on OOM 
// return -errno on failure to write to disk
int UG_sync_blocks_flush_async( struct SG_gateway* gateway, char const* fs_path, uint64_t file_id, int64_t file_version, UG_dirty_block_map_t* dirty_blocks ) {
   
   int rc = 0;
   
   for( UG_dirty_block_map_t::iterator itr = dirty_blocks->begin(); itr != dirty_blocks->end(); itr++ ) {
      
      if( !UG_dirty_block_dirty( &itr->second ) ) {
         
         // no need to flush 
         continue;
      }
      
      if( UG_dirty_block_fd( &itr->second ) >= 0 || UG_dirty_block_is_flushing( &itr->second ) ) {
         
         // already flushed or flushing
         continue;
      }
      
      // start flushing
      rc = UG_dirty_block_flush_async( gateway, fs_path, file_id, file_version, &itr->second );
      if( rc != 0 ) {

         SG_error("UG_dirty_block_flush_async( %" PRIX64 ".%" PRId64 "[%" PRIu64 ".%" PRId64 "] ) rc = %d\n", 
                  file_id, file_version, UG_dirty_block_id( &itr->second ), UG_dirty_block_version( &itr->second ), rc );
         
         break;
      }
   }
   
   return rc;
}


// wait for flushing a set of blocks to finish
// return 0 on success 
// return -errno on failure to write to disk 
// NOTE: the dirty block's buffer will *NOT* be freed--the caller should call UG_dirty_block_map_free if this is desired.
// This method is idempotent--it can be called multiple times on the same dirty block map, and each block will flush at most once.
int UG_sync_blocks_flush_finish( uint64_t file_id, int64_t file_version, UG_dirty_block_map_t* dirty_blocks ) {
   
   int rc = 0;
   int worst_rc = 0;
   
   // finish writing each block 
   for( UG_dirty_block_map_t::iterator itr = dirty_blocks->begin(); itr != dirty_blocks->end(); itr++ ) {
      
      if( !UG_dirty_block_is_flushing( &itr->second ) ) {
         
         // not flushing 
         continue;
      }
      
      // wait for this block to finish writing
      rc = UG_dirty_block_flush_finish( &itr->second );
      if( rc != 0 ) {
         
         // i.e. can fail with -EINVAL if it is dirty but we didn't get around to flushing it
         if( rc != -EINVAL ) {
            
            SG_error("UG_dirty_block_flush_finish( %" PRIX64 ".%" PRId64 "[%" PRIu64 ".%" PRId64 "] ) rc = %d\n",
                     file_id, file_version, UG_dirty_block_id( &itr->second ), UG_dirty_block_version( &itr->second ), rc );
            
            worst_rc = rc;
         }
         else {
            
            rc = 0;
         }
      }
   }
   
   return worst_rc;
}


// set up a sync context.
// sctx takes ownership of rctx and vctx by shallow-copying them.  The caller should stop using rctx and vctx after this method.
// always succeeds
int UG_sync_context_init( struct UG_sync_context* sctx, struct UG_replica_context* rctx, struct UG_vacuum_context* vctx ) {

   memset( sctx, 0, sizeof(struct UG_sync_context) );
   
   sctx->rctx = rctx;
   sctx->vctx = vctx;
   
   sem_init( &sctx->sem, 0, 0 );
   
   return 0;
}


// free up a sync context
// frees the internal replica context given to it earlier 
// always succeeds 
int UG_sync_context_free( struct UG_sync_context* sctx ) {
   
   UG_replica_context_free( sctx->rctx );
   
   if( sctx->vctx != NULL ) {
      UG_vacuum_context_free( sctx->vctx );
   }
   
   sem_destroy( &sctx->sem );
   
   return 0;
}

// indefinitely try to return dirty blocks to the inode
// this does *NOT* affect the inode's manifest; it simply restores the inode's dirty block map
// sleep a bit between attempts, in the hope that some memory gets freed up 
static int UG_sync_dirty_blocks_return( struct UG_inode* inode, UG_dirty_block_map_t* blocks ) {
   
   int rc = 0;
   
   while( true ) {
      
      rc = UG_inode_dirty_blocks_return( inode, blocks );
      if( rc == -ENOMEM ) {
         
         sleep(1);
         continue;
      }
      else {
         break;
      }
   }
   
   return rc;
}

// merge unreplicated blocks back into the inode, but don't overwrite subsequent writes
// free or absorb dirty blocks; ether way clear out *blocks and their cached data.
// this also restores the inode's manifest with the dirty block info.
// return 0 on success 
static int UG_sync_dirty_blocks_restore( struct SG_gateway* gateway, struct UG_inode* inode, int64_t old_file_version, uint64_t old_file_size, UG_dirty_block_map_t* blocks ) {

   int rc = 0;
   UG_dirty_block_map_t* dirty_blocks = UG_inode_dirty_blocks( inode );
   UG_dirty_block_map_t::iterator tmp_itr;
   UG_dirty_block_map_t::iterator dirty_itr;
   struct ms_client* ms = SG_gateway_ms( gateway );
   struct md_syndicate_cache* cache = SG_gateway_cache( gateway );
   uint64_t blocksize = ms_client_get_volume_size( ms );

   struct md_syndicate_cache* cache = SG_gateway_cache( gateway );

   for( UG_dirty_block_map_t::iterator itr = blocks->begin(); itr != blocks->end(); ) {
   
      uint64_t block_id = itr->first;
      struct UG_dirty_block* block = itr->second;

      // don't include if the file was truncated before we could merge dirty data 
      if( old_file_version != UG_inode_file_version( inode ) ) {
         
         if( block_id * block_size >= (unsigned)old_size ) {
            
            tmp_itr = itr;
            itr++;
            
            new_dirty_blocks->erase( tmp_itr );
            UG_dirty_block_evict_and_free( cache, inode, block );
            
            SG_debug("Skip truncated: %" PRIX64 "[%" PRId64 "]\n", UG_inode_file_id( inode ), block_id ); 
            continue;
         }
      }

      // don't overwrite new dirty blocks 
      dirty_itr = dirty_blocks->find( block_id );
      if( UG_dirty_block_version( &dirty_itr->second ) != UG_dirty_block_version( block ) ) {
      
         SG_debug("Won't overwrite: %" PRIX64 "[%" PRId64 "]\n", UG_inode_file_id( inode ), block_id ); 
         
         tmp_itr = itr;
         itr++;

         blocks->erase( tmp_itr );
         UG_dirty_block_evict_and_free( cache, inode, block );

         continue;
      }

      while( 1 ) {
         // keep trying to insert this dirty block into the manifest, but it won't retain old information (since there is none)
         // this propagates the original block version and hash to the inode manifest.
         SG_debug("Restore %" PRIX64 ".%" PRId64 "[%" PRIu64 ".%" PRId64 "]\n", UG_inode_file_id( inode ), UG_inode_file_version( inode ), UG_dirty_block_id( block ), UG_dirty_block_version( block ) );
         rc = UG_inode_dirty_block_commit( gateway, inode, block );
         if( rc != 0 ) {
            
            // failed 
            SG_error("UG_inode_dirty_block_commit( %" PRIX64 ".%" PRIu64" [%" PRIu64 ".%" PRId64 "] ) rc = %d\n", 
                     UG_inode_file_id( inode ), UG_inode_file_version( inode ), UG_dirty_block_id( block ), UG_dirty_block_version( block ), rc );
      
            sleep(1);
            continue;
         }

         break;
      }

      tmp_itr = itr;
      itr++;

      blocks->erase( tmp_itr );
      UG_dirty_block_evict_and_free( cache, inode, block );
   }

   return 0;
}


// fsync an inode.
// flush all dirty blocks to cache, and replicate both the dirty blocks and the manifest to each RG.
// fent must not be locked
int UG_sync_fsync_ex( struct fskit_core* core, char const* path, struct fskit_entry* fent ) {
   
   int rc = 0;
   UG_dirty_block_map_t* dirty_blocks = SG_safe_new( UG_dirty_block_map_t() );
   UG_dirty_block_map_t* new_dirty_blocks = SG_safe_new( UG_dirty_block_map_t() );
   
   struct UG_inode* inode = NULL;
   bool first_in_line = false;          // if true, sync immediately.  otherwise, wait in line.
   
   struct UG_replica_context* rctx = NULL;
   struct UG_vacuum_context* vctx = NULL;
   struct UG_sync_context sctx;
   
   struct SG_gateway* gateway = (struct SG_gateway*)fskit_core_get_user_data( core );
   struct ms_client* ms = SG_gateway_ms( gateway );
   struct UG_state* ug = (struct UG_state*)SG_gateway_cls( gateway );
   struct SG_manifest* replaced_blocks = NULL;
   
   uint64_t block_size = ms_client_get_volume_blocksize( ms );
   
   int64_t file_version = 0;
   off_t file_size = 0;
   struct timespec old_manifest_modtime;
   
   if( dirty_blocks == NULL || new_dirty_blocks == NULL ) {
      
      SG_safe_delete( dirty_blocks );
      SG_safe_delete( new_dirty_blocks );
      return -ENOMEM;
   }
   
   vctx = SG_CALLOC( struct UG_vacuum_context, 1 );
   if( vctx == NULL ) {
      
      SG_safe_delete( dirty_blocks );
      SG_safe_delete( new_dirty_blocks );
      return -ENOMEM;
   }
   
   rctx = UG_replica_context_new();
   if( rctx == NULL ) {
      
      SG_safe_delete( dirty_blocks );
      SG_safe_delete( new_dirty_blocks );
      SG_safe_free( vctx );
      return -ENOMEM;
   }
   
   fskit_entry_wlock( fent );
   
   inode = (struct UG_inode*)fskit_entry_get_user_data( fent );
   file_version = UG_inode_file_version( inode );
   file_size = fskit_entry_get_size( fent );
   
   // get dirty blocks 
   rc = UG_inode_dirty_blocks_extract_modified( inode, dirty_blocks );
   if( rc != 0 ) {
      
      fskit_entry_unlock( fent );
      
      SG_safe_delete( dirty_blocks );
      SG_safe_delete( new_dirty_blocks );

      SG_error("UG_inode_dirty_blocks_extract_modified('%s') rc = %d\n", path, rc );
      return rc;
   }
   
   // make a replica context, snapshotting this inode's dirty blocks and manifest
   rc = UG_replica_context_init( rctx, ug, path, inode, UG_inode_manifest( inode ), dirty_blocks );
 
   // success?
   if( rc != 0 ) {
      
      // nope 
      UG_sync_dirty_blocks_return( inode, dirty_blocks );
      
      fskit_entry_unlock( fent );
      
      SG_safe_delete( dirty_blocks );
      SG_safe_delete( new_dirty_blocks );
      SG_safe_free( vctx );
      SG_safe_free( rctx );

      SG_error("UG_replica_context_init('%s') rc = %d\n", path, rc );
      return rc;
   }
   
   // make a vacuum context, snapshotting this inode's garbage
   replaced_blocks = UG_inode_replaced_blocks( inode );
   if( replaced_blocks == NULL || SG_manifest_get_block_count( replaced_blocks ) == 0 ) {
      
      // nothing to vacuum 
      vctx = NULL;
   }
   else {

      rc = UG_vacuum_context_init( vctx, ug, path, inode, UG_inode_replaced_blocks( inode ) );
   
      // success?
      if( rc != 0 ) {
      
         // nope 
         UG_sync_dirty_blocks_return( inode, dirty_blocks );
         
         fskit_entry_unlock( fent );
         
         SG_safe_delete( dirty_blocks );
         SG_safe_delete( new_dirty_blocks );
         
         UG_replica_context_free( rctx );
         SG_safe_free( rctx );
         SG_safe_free( vctx );

         SG_error("UG_vacuum_context_init('%s') rc = %d\n", path, rc );
         return rc;
      }
   }
   
   // make a sync context...
   UG_sync_context_init( &sctx, rctx, vctx );
   
   // can we sync immediately after unlocking, or do we have to wait?
   if( UG_inode_sync_queue_len( inode ) == 0 ) {
    
      // yup!
      first_in_line = true;
   }
   else {
      
      // wait 
      rc = UG_inode_sync_queue_push( inode, &sctx );
      if( rc != 0 ) {
         
         // OOM
         UG_sync_dirty_blocks_return( inode, dirty_blocks );
         
         fskit_entry_unlock( fent );
         
         SG_safe_delete( dirty_blocks );
         SG_safe_delete( new_dirty_blocks );
         
         UG_replica_context_release_blocks( rctx );
         UG_replica_context_free( rctx );
         SG_safe_free( rctx );
        
         if( vctx == NULL ) { 
             UG_vacuum_context_free( vctx );
             SG_safe_free( vctx );
         }
         
         SG_error("UG_inode_sync_queue_path('%s') rc = %d\n", path, rc );
         return rc;
      }
   }
   
   // replace dirty blocks--we're replicating them now.
   UG_inode_replace_dirty_blocks( inode, new_dirty_blocks );
   
   // clear out replaced block info--we're vacuuming them now.
   UG_inode_clear_replaced_blocks( inode );
   
   // all manifest blocks are now clean--subsequent manifest refreshes can overwrite them
   SG_manifest_set_blocks_dirty( UG_inode_manifest( inode ), false );
   
   // reference this inode--make sure it doesn't get deleted till we're done
   fskit_entry_ref_entry( fent );
   
   fskit_entry_unlock( fent );
   
   // do we have to wait?
   if( !first_in_line ) {
      
      // wait our turn 
      sem_wait( &sctx.sem );
   }
   
   // replicate!
   rc = UG_replicate( gateway, rctx );
   
   // reacquire
   fskit_entry_wlock( fent );
   
   inode = (struct UG_inode*)fskit_entry_get_user_data( fent );
   
   if( rc != 0 ) {
      
      // failed to replicate (i.e. only partially replicated)
      SG_error("UG_replicate( %" PRIX64 ".%" PRId64 " ) rc = %d\n", UG_inode_file_id( inode ), UG_inode_file_version( inode ), rc );
      
      // preserve dirty but uncommitted, non-overwritten blocks
      UG_sync_dirty_blocks_restore( gateway, inode, file_version, file_size, UG_replica_context_blocks( rctx ) );
      
      // put back vacuum state into the inode 
      if( vctx != NULL ) {
         rc = UG_vacuum_context_restore( vctx, inode );
         if( rc != 0 ) {
            
            SG_error("UG_vacuum_context_restore( %" PRIX64 ".%" PRId64 " ) rc = %d\n", UG_inode_file_id( inode ), UG_inode_file_version( inode ), rc );
            
            // not only did we partially replicate, we don't remember which blocks we need to try again!
            // the only real solution (long-run) is to start up a new coordinator for this file and have it vacuum it
            // (or at some point, I'll write an fsck-like tool for Syndicate that reclaims leaked blocks)
         }
         
         // this is an I/O error 
         rc = -EIO;
         
         UG_vacuum_context_free( vctx );
         SG_safe_free( vctx );
      }
   }
   else {
      
      // success!  this manifest is the last successfully-vacuumed manifest
      old_manifest_modtime.tv_sec = UG_replica_context_inode_data( rctx )->manifest_mtime_sec;
      old_manifest_modtime.tv_nsec = UG_replica_context_inode_data( rctx )->manifest_mtime_nsec;
      
      UG_inode_set_old_manifest_modtime( inode, &old_manifest_modtime );
      
      while( 1 ) {
         
         // begin vacuuming the old manifest
         // can only fail with ENOMEM, in which case we need to try again
         rc = UG_vacuumer_enqueue( UG_state_vacuumer( ug ), vctx );
         if( rc != 0 ) {
            
            SG_error("UG_vacuumer_enqueue( %" PRIX64 ".%" PRId64 " ) rc = %d\n", UG_inode_file_id( inode ), UG_inode_file_version( inode ), rc );
            
            continue;
         }

         break;
      }
   }
   
   // wake up the next sync request
   if( UG_inode_sync_queue_len( inode ) > 0 ) {
      
      struct UG_sync_context* sctx_ptr = UG_inode_sync_queue_pop( inode );
      if( sctx_ptr != NULL ) {
         
         sem_post( &sctx_ptr->sem );
      }
   }
   
   fskit_entry_unlock( fent );
   
   UG_replica_context_free( rctx );
   SG_safe_free( rctx );
   
   fskit_entry_unref( core, path, fent );
   
   return rc;
}

// fskit fsync
int UG_sync_fsync( struct fskit_core* core, struct fskit_route_metadata* route_metadata, struct fskit_entry* fent ) {
   
   return UG_sync_fsync_ex( core, fskit_route_metadata_get_path( route_metadata ), fent );
}
