/*
   Copyright 2013 The Trustees of Princeton University
   All Rights Reserved
*/

#include "readdir.h"

// low-level read directory
// dent must be read-locked
struct fs_dir_entry** fs_entry_readdir_lowlevel( struct fs_core* core, char const* fs_path, struct fs_entry* dent ) {
   unsigned int num_ents = fs_entry_set_count( dent->children );

   struct fs_dir_entry** dents = (struct fs_dir_entry**)calloc( sizeof(struct fs_dir_entry*) * (num_ents + 1), 1 );
   unsigned int cnt = 0;

   
   for( fs_entry_set::iterator itr = dent->children->begin(); itr != dent->children->end(); itr++ ) {
      struct fs_entry* fent = fs_entry_set_get( &itr );
      long fent_name_hash = fs_entry_set_get_name_hash( &itr );

      if( fent == NULL )
         continue;

      struct fs_dir_entry* d_ent = NULL;

      // handle . and .. separately--we only want to lock children (not the current or parent directory)
      if( fent_name_hash == fs_entry_name_hash( "." ) ) {
         d_ent = CALLOC_LIST( struct fs_dir_entry, 1 );
         d_ent->ftype = FTYPE_DIR;
         fs_entry_to_md_entry( core, ".", dent, &d_ent->data );
      }
      else if( fent_name_hash == fs_entry_name_hash( ".." ) ) {
         d_ent = CALLOC_LIST( struct fs_dir_entry, 1 );
         d_ent->ftype = FTYPE_DIR;
         char* parent_path = md_dirname( fs_path, NULL );

         if( fent != dent )
            fs_entry_to_md_entry( core, parent_path, SYS_USER, dent->volume, &d_ent->data );
         else
            fs_entry_to_md_entry( core, "..", dent, &d_ent->data );     // this is /

         // change path to ..
         free( d_ent->data.path );
         d_ent->data.path = strdup("..");

         free( parent_path );
      }
      else {
         if( fent != dent && fs_entry_rlock( fent ) != 0 ) {
            continue;
         }
         else if( fent->name != NULL ) {
            d_ent = CALLOC_LIST( struct fs_dir_entry, 1 );
            d_ent->ftype = fent->ftype;
            fs_entry_to_md_entry( core, fent->name, fent, &d_ent->data );

            if( fent != dent )
               fs_entry_unlock( fent );
         }
         else {

            if( fent != dent )
               fs_entry_unlock( fent );

            continue;
         }
      }

      if( d_ent != NULL ) {
         dents[cnt] = d_ent;
         dbprintf( "in %s: %s\n", dent->name, d_ent->data.path );
         cnt++;
      }
   }
   
   return dents;
}


// read data from a directory
struct fs_dir_entry** fs_entry_readdir( struct fs_core* core, struct fs_dir_handle* dirh, int* err ) {
   fs_dir_handle_rlock( dirh );
   if( dirh->dent == NULL || dirh->open_count <= 0 ) {
      // invalid
      fs_dir_handle_unlock( dirh );
      *err = -EBADF;
      return NULL;
   }

   fs_entry_rlock( dirh->dent );

   struct fs_dir_entry** dents = fs_entry_readdir_lowlevel( core, dirh->path, dirh->dent );

   fs_entry_unlock( dirh->dent );
   
   fs_dir_handle_unlock( dirh );

   return dents;
}
