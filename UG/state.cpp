/*
   Copyright 2013 The Trustees of Princeton University

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

#include "state.h"

// initialize the state
int syndicate_init_state( struct syndicate_state* state, struct ms_client* ms ) {
   
   int rc = 0;
   
   state->ms = ms;
   
   // get the volume
   uint64_t volume_id = ms_client_get_volume_id( state->ms );
   uint64_t blocking_factor = ms_client_get_volume_blocksize( state->ms );

   if( volume_id == 0 ) {
      errorf("%s", "Volume not found\n");
      return -ENOENT;
   }
   
   // make the logfile
   state->logfile = log_init( state->conf.logfile_path );
   if( state->logfile == NULL ) {
      return -ENOMEM;
   }

   // start up stats gathering
   state->stats = new Stats( NULL );
   state->stats->use_conf( &state->conf );

   // get root info
   struct md_entry root;
   memset( &root, 0, sizeof(root) );

   rc = ms_client_get_volume_root( state->ms, &root );
   if( rc != 0 ) {
      errorf("ms_client_get_volume_root rc = %d\n", rc );
      return -ENODATA;
   }

   // sanity check
   if( root.volume != volume_id ) {
      errorf("Invalid root Volume %" PRIu64 "\n", root.volume );
      md_entry_free( &root );
      return -EINVAL;
   }

   // initialize the filesystem core
   struct fs_core* core = CALLOC_LIST( struct fs_core, 1 );
   fs_core_init( core, &state->conf, root.owner, root.coordinator, root.volume, root.mode, blocking_factor );

   md_entry_free( &root );

   fs_entry_set_config( &state->conf );

   state->core = core;
   state->col = new Collator( core );
   
   fs_core_use_collator( core, state->col );
   fs_core_use_ms( core, state->ms );
   fs_core_use_state( core, state );

   // restore local files
   rc = fs_entry_restore_files( core );
   if( rc != 0 ) {
      errorf("fs_entry_restore_files rc = %d\n", rc );
      exit(1);
   }

   state->col->start();
   
   state->uid = getuid();
   state->gid = getgid();
   
   state->mounttime = currentTimeSeconds();

   // start up replication
   replication_init( state, volume_id );

   return 0;
}

int syndicate_set_running( struct syndicate_state* state, int running ) {
   state->running = running;
   return 0;
}

int syndicate_destroy_state( struct syndicate_state* state, int wait_replicas ) {
   
   state->running = 0;

   dbprintf("%s", "stopping replication\n");
   replication_shutdown( state, wait_replicas );
   
   dbprintf("%s", "destroy collation\n");
   delete state->col;

   dbprintf("%s", "destory MS client\n");
   ms_client_destroy( state->ms );
   free( state->ms );

   dbprintf("%s", "core filesystem shutdown\n");
   fs_destroy( state->core );
   free( state->core );
   
   string statistics_str = state->stats->dump();
   printf("Statistics: \n%s\n", statistics_str.c_str() );
   delete state->stats;

   dbprintf("%s", "log shutdown\n");

   log_shutdown( state->logfile );

   dbprintf("%s", "free configuration\n");
   md_free_conf( &state->conf );
   
   return 0;
}