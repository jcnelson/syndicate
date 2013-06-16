/*
   Copyright 2013 The Trustees of Princeton University
   All Rights Reserved
*/


#include "trunc.h"
#include "storage.h"
#include "manifest.h"
#include "url.h"
#include "network.h"
#include "stat.h"
#include "read.h"
#include "replication.h"
#include "collator.h"

static void fs_entry_prepare_truncate_message( Serialization::WriteMsg* truncate_msg, char const* fs_path, struct fs_entry* fent, uint64_t new_max_block ) {
   Serialization::TruncateRequest* truncate_req = truncate_msg->mutable_truncate();
   truncate_req->set_fs_path( fs_path );
   truncate_req->set_file_version( fent->version );
   truncate_req->set_size( fent->size );

   Serialization::BlockList* blocks = truncate_msg->mutable_blocks();
   blocks->set_start_id( 0 );
   blocks->set_end_id( new_max_block );

   for( uint64_t i = 0; i < new_max_block; i++ ) {
      int64_t block_version = fent->manifest->get_block_version( i );

      blocks->add_version( block_version );
   }
}

                                               
// truncate an open file.
// fent must be write locked.
// NOTE: we must reversion the file on truncate, since size can't decrease on the MS for the same version of the entry!
int fs_entry_truncate_impl( struct fs_core* core, char const* fs_path, struct fs_entry* fent, off_t size, uid_t user, gid_t volume ) {

   // make sure we have the latest manifest 
   int err = fs_entry_revalidate_manifest( core, fs_path, fent );
   if( err != 0 ) {
      errorf( "fs_entry_revalidate_manifest(%s) rc = %d\n", fs_path, err );
      fs_entry_unlock( fent );
      return err;
   }

   uint64_t trunc_block_id = 0;
   
   // which blocks need to be withdrawn?
   uint64_t max_block = fent->size / core->conf->blocking_factor;
   if( fent->size % core->conf->blocking_factor > 0 ) {
      max_block++;      // preserve the remainder of the last block
   }

   uint64_t new_max_block = size / core->conf->blocking_factor;
   if( size % core->conf->blocking_factor > 0 ) {
      trunc_block_id = new_max_block;      // need to truncate the last block
      new_max_block++;
   }

   // which blocks are modified?
   modification_map modified_blocks;

   // are we going to lose any remote blocks?
   bool local = URL_LOCAL( fent->url );

   // if we're removing blocks, then we'll need to withdraw them.
   if( size < fent->size ) {

      if( trunc_block_id > 0 ) {
         // truncate the last block
         char* block = CALLOC_LIST( char, core->conf->blocking_factor );

         ssize_t nr = fs_entry_do_read_block( core, fs_path, fent, trunc_block_id * core->conf->blocking_factor, block );
         if( nr < 0 ) {
            errorf( "fs_entry_do_read_block(%s[%" PRIu64 "]) rc = %zd\n", fs_path, trunc_block_id, nr );
            err = nr;
         }
         else {
            // truncate this block
            memset( block + (size % core->conf->blocking_factor), 0, core->conf->blocking_factor - (size % core->conf->blocking_factor) );

            int rc = fs_entry_put_block( core, fs_path, fent, trunc_block_id, block );
            if( rc != 0 ) {
               errorf("fs_entry_put_block(%s[%" PRId64 "]) rc = %d\n", fs_path, trunc_block_id, rc );
               err = rc;
            }
            else {
               // record that we've written this block
               modified_blocks[ trunc_block_id ] = fent->manifest->get_block_version( trunc_block_id );
            }
         }

         free( block );
      }
      
      if( local ) {
         // unlink the blocks that would have been cut off
         for( uint64_t i = new_max_block; i < max_block; i++ ) {
            int rc = fs_entry_remove_block( core, fs_path, fent, i );
            if( rc != 0 && rc != -ENOENT ) {
               errorf("fs_entry_remove_block(%s.%" PRId64 "[%" PRIu64 "]) rc = %d\n", fs_path, fent->version, i, rc );
            }
         }

         fent->manifest->truncate( new_max_block );
      }

      if( err == 0 )
         fent->size = size;
      
   }
   else if( size > fent->size ) {

      int rc = fs_entry_expand_file( core, fs_path, fent, size, &modified_blocks );
      if( rc != 0 ) {
         errorf("fs_entry_expand_file(%s) rc = %d\n", fs_path, rc );
         err = rc;
      }
   }

   // inform the remote block owner that the data must be truncated
   if( err == 0 && !local ) {

      // build up a truncate write message
      Serialization::WriteMsg *truncate_msg = new Serialization::WriteMsg();
      fs_entry_init_write_message( truncate_msg, core, Serialization::WriteMsg::TRUNCATE );
      fs_entry_prepare_truncate_message( truncate_msg, fs_path, fent, new_max_block );

      Serialization::WriteMsg *withdraw_ack = new Serialization::WriteMsg();
      
      err = fs_entry_post_write( withdraw_ack, core, fent->url, truncate_msg );

      if( err != 0 ) {
         errorf( "fs_entry_post_write(%" PRIu64 "-%" PRIu64 ") rc = %d\n", new_max_block, max_block, err );
         err = -EIO;
      }
      else if( withdraw_ack->type() != Serialization::WriteMsg::ACCEPTED ) {
         if( withdraw_ack->type() == Serialization::WriteMsg::ERROR ) {
            errorf( "remote truncate failed, error = %d (%s)\n", withdraw_ack->errorcode(), withdraw_ack->errortxt().c_str() );
            err = withdraw_ack->errorcode();
         }
         else {
            errorf( "remote truncate invalid message %d\n", withdraw_ack->type() );
            err = -EIO;
         }
      }
      else {
         // success!
         err = 0;
      }
      
      delete withdraw_ack;
      delete truncate_msg;

      // the remote host will have reversioned the file.
      // we need to refresh its metadata before then.
      fs_entry_mark_read_stale( fent );
   }


   // replicate if the file is local
   if( err == 0 && local && modified_blocks.size() > 0 ) {
      // stop collating the affected blocks
      uint64_t cancel_block_start = modified_blocks.begin()->first;
      uint64_t cancel_block_end = modified_blocks.rbegin()->first + 1;     // exclusive
      
      // TODO: less hackish way of doing this?  Only works because fs_entry_replicate_write returns after fh gets cleared from the replication thread
      struct fs_file_handle fh;
      fh.fent = fent;
      fh.path = (char*)fs_path;
      int rc = fs_entry_replicate_write( core, &fh, &modified_blocks, true );
      if( rc != 0 ) {
         errorf("fs_entry_replicate_write(%s[%" PRId64 "-%" PRId64 "]) rc = %d\n", fs_path, cancel_block_start, cancel_block_end, rc );
      }
   }

   // reversion this file atomically
   if( err == 0 && local ) {

      int64_t new_version = fs_entry_next_file_version();

      err = fs_entry_reversion_file( core, fs_path, fent, new_version );

      if( err != 0 ) {
         errorf("fs_entry_reversion_file(%s.%" PRId64 " --> %" PRId64 ") rc = %d\n", fs_path, fent->version, new_version, err );
      }
   }

   return err;
}



// truncate, only if the version is correct (or ignore it if it's -1)
int fs_entry_versioned_truncate(struct fs_core* core, const char* fs_path, off_t newsize, int64_t known_version, uid_t user, gid_t volume ) {

   int err = fs_entry_revalidate_path( core, fs_path );
   if( err != 0 ) {
      errorf("fs_entry_revalidate_path(%s) rc = %d\n", fs_path, err );
      return -EREMOTEIO;
   }

   // entry exists
   // write-lock the fs entry
   struct fs_entry* fent = fs_entry_resolve_path( core, fs_path, user, volume, true, &err );
   if( fent == NULL || err ) {
      errorf( "fs_entry_resolve_path(%s), rc = %d\n", fs_path, err );
      return err;
   }
   
   if( known_version > 0 && fent->version != known_version ) {
      errorf( "fs_entry_get_version(%s): version mismatch (current = %" PRId64 ", known = %" PRId64 ")\n", fs_path, fent->version, known_version );
      fs_entry_unlock( fent );
      return -EINVAL;
   }

   int rc = fs_entry_truncate_impl( core, fs_path, fent, newsize, user, volume );
   if( rc != 0 ) {
      errorf( "fs_entry_truncate(%s) rc = %d\n", fs_path, rc );

      fs_entry_unlock( fent );
      return rc;
   }

   fs_entry_unlock( fent );

   return rc;
}


// truncate an file
int fs_entry_truncate( struct fs_core* core, char const* fs_path, off_t size, uid_t user, gid_t volume ) {
   
   int err = fs_entry_revalidate_path( core, fs_path );
   if( err != 0 ) {
      errorf("fs_entry_revalidate_path(%s) rc = %d\n", fs_path, err );
      return -EREMOTEIO;
   }

   // entry exists
   // write-lock the fs entry
   struct fs_entry* fent = fs_entry_resolve_path( core, fs_path, user, volume, true, &err );
   if( fent == NULL || err ) {
      errorf( "fs_entry_resolve_path(%s), rc = %d\n", fs_path, err );
      return err;
   }
   
   err = fs_entry_truncate_impl( core, fs_path, fent, size, user, volume );

   fs_entry_unlock( fent );

   return err;
}

// truncate a file
int fs_entry_ftruncate( struct fs_core* core, struct fs_file_handle* fh, off_t size, uid_t user, gid_t volume ) {
   fs_file_handle_rlock( fh );
   fs_entry_wlock( fh->fent );

   int rc = fs_entry_truncate_impl( core, fh->path, fh->fent, size, user, volume );
   
   fs_entry_unlock( fh->fent );
   fs_file_handle_unlock( fh );
   return rc;
}