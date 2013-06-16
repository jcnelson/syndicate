/*
   Copyright 2013 The Trustees of Princeton University
   All Rights Reserved
*/

#include "closedir.h"


// close a directory handle
int fs_entry_closedir( struct fs_core* core, struct fs_dir_handle* dirh ) {
   fs_dir_handle_wlock( dirh );

   fs_dir_handle_close( dirh );

   if( dirh->open_count <= 0 ) {
      // all references to this handle are gone.
      // decrement reference count
      fs_entry_wlock( dirh->dent );

      dirh->dent->open_count--;

      if( dirh->dent->open_count <= 0 && dirh->dent->link_count <= 0 ) {
         fs_entry_destroy( dirh->dent, false );
         free( dirh->dent );
         dirh->dent = NULL;
      }
      else {
         fs_entry_unlock( dirh->dent );
      }

      fs_dir_handle_destroy( dirh );
   }
   else {
      fs_dir_handle_unlock( dirh );
   }

   return 0;
}

// close a directory handle
// NOTE: make sure everything's locked first!
int fs_dir_handle_close( struct fs_dir_handle* dh ) {
   dh->open_count--;
   return 0;
}
