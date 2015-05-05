/*
   Copyright 2014 The Trustees of Princeton University

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

#include "libsyndicate/closure.h"
#include "libsyndicate/crypt.h"

// free a callback table 
// always succeeds
static void md_closure_callback_table_free( struct md_closure_callback_entry* callbacks ) {
   
   for( int i = 0; callbacks[i].sym_name != NULL; i++ ) {
      
      if( callbacks[i].sym_name ) {
         
         SG_safe_free( callbacks[i].sym_name );
      }
   }
}

// duplicate a callback table 
// return the new table on success
// return NULL on OOM
static struct md_closure_callback_entry* md_closure_callback_table_from_prototype( struct md_closure_callback_entry* prototype ) {
   
   // count them...
   int num_cbs = 0;
   for( int i = 0; prototype[i].sym_name != NULL; i++ ) {
      num_cbs++;
   }
   
   struct md_closure_callback_entry* ret = SG_CALLOC( struct md_closure_callback_entry, num_cbs + 1 );
   if( ret == NULL ) {
      return NULL;
   }
   
   for( int i = 0; prototype[i].sym_name != NULL; i++ ) {
      ret[i].sym_name = SG_strdup_or_null( prototype[i].sym_name );
      ret[i].sym_ptr = NULL;
      
      if( ret[i].sym_name == NULL ) {
         
         md_closure_callback_table_free( ret );
         SG_safe_free( ret );
         break;
      }
   }
   
   return ret;
}

// load a string as a JSON object 
// return 0 on success, and fill in *jobj_ret 
// return -ENOMEM on OOM
// return -EINVAL if we failed to parses
static int md_parse_json_object( struct json_object** jobj_ret, char const* obj_json, size_t obj_json_len ) {
   
   char* tmp = SG_CALLOC( char, obj_json_len + 1 );
   if( tmp == NULL ) {
      return -ENOMEM;
   }
   
   memcpy( tmp, obj_json, obj_json_len );
   
   // obj_json should be a valid json string that contains a single dictionary.
   struct json_tokener* tok = json_tokener_new();
   if( tok == NULL ) {
      
      SG_safe_free( tmp );
      return -ENOMEM;
   }
   
   struct json_object* jobj = json_tokener_parse_ex( tok, tmp, obj_json_len );
   
   json_tokener_free( tok );
   
   if( jobj == NULL ) {
      
      SG_error("Failed to parse JSON object %p '%s'\n", obj_json, tmp );
      
      SG_safe_free( tmp );
      
      return -EINVAL;
   }
   
   SG_safe_free( tmp );
   
   // should be an object
   enum json_type jtype = json_object_get_type( jobj );
   if( jtype != json_type_object ) {
      
      SG_error("%s", "JSON config is not a JSON object\n");
      
      json_object_put( jobj );
      return -EINVAL;
   }
   
   *jobj_ret = jobj;
   return 0;
}


// load a base64-encoded string into a JSON object 
// return 0 on success, and set *jobj_ret
// return -ENOMEM on OOM 
// return -EINVAL on failure to parse the string (either from base64 to binary, or from binary to json)
static int md_parse_b64_object( struct json_object** jobj_ret, char const* obj_b64, size_t obj_b64_len ) {
   // deserialize...
   char* obj_json = NULL;
   size_t obj_json_len = 0;
   
   int rc = 0;
   
   rc = md_base64_decode( obj_b64, obj_b64_len, &obj_json, &obj_json_len );
   if( rc != 0 ) {
      
      SG_error("md_base64_decode rc = %d\n", rc );
      
      if( rc != -ENOMEM ) {
         return -EINVAL;
      }
      else {
         return rc;
      }
   }
   
   rc = md_parse_json_object( jobj_ret, obj_json, obj_json_len );
   if( rc != 0 ) {
      
      SG_error("md_parse_json_object rc = %d\n", rc );
   }
   
   SG_safe_free( obj_json );
   
   return rc;
}


// parse the config 
// return 0 on success 
// return -ENOMEM on OOM 
// return -EINVAL on parse error
static int md_parse_closure_config( md_closure_conf_t* closure_conf, char const* closure_conf_b64, size_t closure_conf_b64_len ) {
   
   struct json_object* jobj = NULL;
   
   int rc = md_parse_b64_object( &jobj, closure_conf_b64, closure_conf_b64_len );
   if( rc != 0 ) {
      
      SG_error("Failed to parse JSON object, rc = %d\n", rc );
      return rc;
   }
   
   // iterate through the fields 
   json_object_object_foreach( jobj, key, val ) {
      
      // each field needs to be a string...
      enum json_type jtype = json_object_get_type( val );
      if( jtype != json_type_string ) {
         
         SG_error("%s is not a JSON string\n", key );
         rc = -EINVAL;
         break;
      }
      
      // get the value 
      char const* value = json_object_get_string( val );
      if( value == NULL ) {
         
         // OOM 
         rc = -ENOMEM;
         break;
      }
      
      size_t value_len = strlen(value);         // json_object_get_string_len( val );
      
      try {
         // put it into the config 
         string key_s( key );
         string value_s( value, value_len );
         
         (*closure_conf)[ key_s ] = value_s;
      }
      catch( bad_alloc& ba ) {
         
         // OOM 
         rc = -ENOMEM;
         break;
      }
   }
   
   // done with this 
   json_object_put( jobj );
   
   return rc;
}


// decrypt secrets and put the plaintext into an mlock'ed buffer 
// return 0 on success
// return -ENOMEM on OOM 
// return -EINVAL on failure to parse
static int md_decrypt_secrets( EVP_PKEY* gateway_pubkey, EVP_PKEY* gateway_pkey, struct json_object** jobj, char const* closure_secrets_b64, size_t closure_secrets_b64_len ) {
   
   // deserialize...
   char* obj_ctext = NULL;
   size_t obj_ctext_len = 0;
   
   int rc = 0;
   
   rc = md_base64_decode( closure_secrets_b64, closure_secrets_b64_len, &obj_ctext, &obj_ctext_len );
   if( rc != 0 ) {
      
      SG_error("md_base64_decode rc = %d\n", rc );
      return -EINVAL;
   }
   
   // decrypt...
   char* obj_json = NULL;
   size_t obj_json_len = 0;
   
   rc = md_decrypt( gateway_pubkey, gateway_pkey, obj_ctext, obj_ctext_len, &obj_json, &obj_json_len );
   
   SG_safe_free( obj_ctext );
   
   if( rc != 0 ) {
      
      SG_error("md_decrypt rc = %d\n", rc );
      return -EINVAL;
   }
   
   // parse 
   rc = md_parse_json_object( jobj, obj_json, obj_json_len );
   SG_safe_free( obj_json );
   
   if( rc != 0 ) {
      SG_error("md_parse_json_object rc = %d\n", rc );
   }
   
   return rc;
}

// parse the secrets 
// return 0 on success
// return -ENOMEM on OOM 
// return -EINVAL on failure to parse
static int md_parse_closure_secrets( EVP_PKEY* gateway_pubkey, EVP_PKEY* gateway_pkey, md_closure_secrets_t* closure_secrets, char const* closure_secrets_b64, size_t closure_secrets_b64_len ) {
   
   struct json_object* jobj = NULL;
   
   int rc = md_decrypt_secrets( gateway_pubkey, gateway_pkey, &jobj, closure_secrets_b64, closure_secrets_b64_len );
   if( rc != 0 ) {
      
      SG_error("Failed to decrypt, rc = %d\n", rc );
      return rc;
   }
   
   // iterate through the fields 
   json_object_object_foreach( jobj, key, val ) {
      
      // each field needs to be a string...
      enum json_type jtype = json_object_get_type( val );
      if( jtype != json_type_string ) {
         SG_error("%s is not a JSON string\n", key );
         rc = -EINVAL;
         break;
      }
      
      // get the value 
      char const* encrypted_value = json_object_get_string( val );
      if( encrypted_value == NULL ) {
         
         rc = -ENOMEM;
         break;
      }
      
      size_t encrypted_value_len = strlen(encrypted_value);
      
      try {
         // put it into the secrets
         string key_s( key );
         string value_s( encrypted_value, encrypted_value_len );
         
         (*closure_secrets)[ key_s ] = value_s;
      }
      catch( bad_alloc& ba ) {
         
         rc = -ENOMEM;
         break;
      }
   }
   
   // done with this 
   json_object_put( jobj );
   
   return rc;
}

// load a string by key 
// returns a reference to the value in the json object on success (do NOT free or modify it)
// return NULL if not found, or if OOM
static char const* md_load_json_string_by_key( struct json_object* obj, char const* key, size_t* _val_len ) {
   
   // look up the keyed value
   // TODO: use json_object_object_get_ex at some point, when we can move away from old libjson
   struct json_object* key_obj = NULL;
   
   key_obj = json_object_object_get( obj, key );
   if( key_obj == NULL ) {
      
      SG_error("No such key '%s'\n", key );
      return NULL;
   }
   
   // verify it's a string 
   enum json_type jtype = json_object_get_type( key_obj );
   if( jtype != json_type_string ) {
      
      SG_error("'%s' is not a string\n", key );
      return NULL;
   }
   
   char const* val = json_object_get_string( key_obj );
   if( val == NULL ) {
      
      // OOM
      return NULL;
   }
   
   *_val_len = strlen(val);  // json_object_get_string_len( val );
   return val;
}


// load a chunk of data by key directly 
// return 0 on success, and set *val and *val_len to the value
// return -ENOENT if there is no such key 
// return -EINVAL on parse error 
// return -ENOMEM if OOM
static int md_parse_json_b64_string( struct json_object* toplevel_obj, char const* key, char** val, size_t* val_len ) {
   int rc = 0;
   
   // look up the keyed value
   size_t b64_len = 0;
   char const* b64 = md_load_json_string_by_key( toplevel_obj, key, &b64_len );
   
   if( b64 == NULL || b64_len == 0 ) {
      
      SG_error("No value for '%s'\n", key);
      rc = -ENOENT;
   }
   else {
      
      char* tmp = NULL;
      size_t tmp_len = 0;
      
      // load it directly...
      rc = md_base64_decode( b64, b64_len, &tmp, &tmp_len );
      if( rc != 0 ) {
         
         SG_error("md_base64_decode('%s') rc = %d\n", key, rc );
      }
      else {
         
         *val = tmp;
         *val_len = tmp_len;
      }
   }
   
   return rc;
}


// parse the MS-supplied closure.
// return 0 on success
// return -ENOMEM on OOM
static int md_parse_closure( md_closure_conf_t* closure_conf,
                             EVP_PKEY* pubkey, EVP_PKEY* privkey, md_closure_secrets_t* closure_secrets,
                             char** driver_bin, size_t* driver_bin_len,
                             char const* closure_text, size_t closure_text_len ) {
      
   // closure_text should be a JSON object...
   struct json_object* toplevel_obj = NULL;
   
   int rc = md_parse_json_object( &toplevel_obj, closure_text, closure_text_len );
   if( rc != 0 ) {
      
      SG_error("md_parse_json_object rc = %d\n", rc );
      return -EINVAL;
   }
   
   // requested config?
   if( rc == 0 && closure_conf ) {
      
      // get the closure conf JSON 
      size_t json_b64_len = 0;
      char const* json_b64 = md_load_json_string_by_key( toplevel_obj, "config", &json_b64_len );
      
      if( json_b64 != NULL && json_b64_len != 0 ) {
         
         // load it
         rc = md_parse_closure_config( closure_conf, json_b64, json_b64_len );
         if( rc != 0 ) {
            
            SG_error("md_parse_closure_config rc = %d\n", rc );
         }
      }
   }
   
   // requested secrets?
   if( rc == 0 && closure_secrets ) {
      
      // get the closure secrets JSON 
      size_t json_b64_len = 0;
      char const* json_b64 = md_load_json_string_by_key( toplevel_obj, "secrets", &json_b64_len );
      
      if( json_b64 != NULL || json_b64_len != 0 ) {
         
         // load it 
         rc = md_parse_closure_secrets( pubkey, privkey, closure_secrets, json_b64, json_b64_len );
         if( rc != 0 ) {
            SG_error("md_parse_closure_config rc = %d\n", rc );
         }
      }
   }
   
   // requested driver (or specfile)?
   if( rc == 0 && driver_bin != NULL && driver_bin_len != NULL ) {
      
      rc = md_parse_json_b64_string( toplevel_obj, "driver", driver_bin, driver_bin_len );
   
      // not an error if not present...
      if( rc == -ENOENT ) {
         rc = 0;
      }
   }
   
   if( rc != 0 ) {
      
      // failed somewhere...clean up
      SG_safe_free( *driver_bin );
      
      if( closure_conf ) {
         closure_conf->clear();
      }
      if( closure_secrets ) {
         closure_secrets->clear();
      }
   }
   
   json_object_put( toplevel_obj );
   
   return rc;
}


// initialize a closure.
// validate it using the given public key.
// decrypt the closure secrets using the private key.
// load symbols as defined by driver_prototype.
// return 0 on success, and populate *closure 
// return -ENOMEM on OOM
int md_closure_init( struct md_closure* closure,
                     struct md_syndicate_conf* conf,
                     EVP_PKEY* pubkey, EVP_PKEY* privkey,
                     struct md_closure_callback_entry* driver_prototype,
                     char const* closure_text, size_t closure_text_len,
                     bool ignore_stubs ) {
   
   memset( closure, 0, sizeof(struct md_closure) );
   
   md_closure_conf_t* closure_conf = SG_safe_new( md_closure_conf_t() );
   md_closure_secrets_t* closure_secrets = SG_safe_new( md_closure_secrets_t() );
   
   if( closure_conf == NULL || closure_secrets == NULL ) {
      
      SG_safe_delete( closure_conf );
      SG_safe_delete( closure_secrets );
      return -ENOMEM;
   }
   
   char* driver_bin = NULL;
   size_t driver_bin_len = 0;
   
   // load up the config, secrets, and driver
   int rc = md_parse_closure( closure_conf, pubkey, privkey, closure_secrets, &driver_bin, &driver_bin_len, closure_text, closure_text_len );
   if( rc != 0 ) {
      
      SG_error("md_parse_closure rc = %d\n", rc );
      
      SG_safe_delete( closure_conf );
      SG_safe_delete( closure_secrets );
      
      return rc;
   }
   
   // intialize the closure 
   rc = pthread_rwlock_init( &closure->reload_lock, NULL );
   if( rc != 0 ) {
      
      SG_safe_delete( closure_conf );
      SG_safe_delete( closure_secrets );
      SG_safe_free( driver_bin );
      return rc;
   }
   
   // initialize the callbacks from the prototype
   closure->callbacks = md_closure_callback_table_from_prototype( driver_prototype );
   if( closure->callbacks == NULL ) {
      
      // OOM 
      SG_safe_delete( closure_conf );
      SG_safe_delete( closure_secrets );
      SG_safe_free( driver_bin );
      pthread_rwlock_destroy( &closure->reload_lock );
      
      return -ENOMEM;
   }
   
   // load the information into the closure 
   closure->closure_conf = closure_conf;
   closure->closure_secrets = closure_secrets;
   closure->ignore_stubs = ignore_stubs;
   
   // initialize the driver
   rc = md_closure_driver_reload( conf, closure, driver_bin, driver_bin_len );
   if( rc != 0 ) {
      SG_error("md_closure_driver_reload rc = %d\n", rc );
      
      md_closure_shutdown( closure );
   }
   else {
      
      // ready to roll!
      closure->running = 1;
   }
   
   SG_safe_free( driver_bin );
   
   return rc;
}


// parse an AG's specfile, given its json-encoded form.
// TODO: deprecate
int md_closure_load_AG_specfile( char* specfile_json, size_t specfile_json_len, char** specfile_text, size_t* specfile_text_len ) {
   
   struct json_object* toplevel_obj = NULL;
   
   int rc = md_parse_json_object( &toplevel_obj, specfile_json, specfile_json_len );
   if( rc != 0 ) {
      SG_error("md_parse_json_object rc = %d\n", rc );
      return -EINVAL;
   }
   
   rc = md_parse_json_b64_string( toplevel_obj, "spec", specfile_text, specfile_text_len );
   if( rc != 0 ) {
      SG_error("md_parse_json_b64_string rc = %d\n", rc );
   }
   
   json_object_put( toplevel_obj );
   
   return rc;
}


// initialize a closure from an on-disk .so file.
// do not bother trying to load configuration or secrets
// return 0 on success 
// return -ENOMEM on OOM
int md_closure_init_bin( struct md_syndicate_conf* conf, struct md_closure* closure, char const* so_path, struct md_closure_callback_entry* driver_prototype, bool ignore_stubs ) {
   
   memset( closure, 0, sizeof(struct md_closure));
   
   // intialize the closure 
   int rc = pthread_rwlock_init( &closure->reload_lock, NULL );
   if( rc != 0 ) {
      
      return rc;
   }
   
   closure->callbacks = md_closure_callback_table_from_prototype( driver_prototype );
   if( closure->callbacks == NULL ) {
      
      pthread_rwlock_destroy( &closure->reload_lock );
      return -ENOMEM;
   }
   
   closure->so_path = SG_strdup_or_null( so_path );
   if( closure->so_path == NULL ) {
      
      pthread_rwlock_destroy( &closure->reload_lock );
      md_closure_callback_table_free( closure->callbacks );
      
      SG_safe_free( closure->callbacks );
      return -ENOMEM;
   }
      
      
   closure->ignore_stubs = ignore_stubs;
   closure->on_disk = true;
   
   // initialize the driver
   rc = md_closure_driver_reload( conf, closure, NULL, 0 );
   if( rc != 0 ) {
      
      SG_error("md_closure_driver_reload rc = %d\n", rc );
      
      md_closure_shutdown( closure );
   }
   else {
      // ready to roll!
      closure->running = 1;
   }
   
   return 0;
}


// write the MS-supplied closure to a temporary file
// return 0 on success, and set *_so_path_ret to the path to the driver 
// return -ENOMEM on OOM 
// return -errno on failure to write
int md_write_driver( struct md_syndicate_conf* conf, char** _so_path_ret, char const* driver_text, size_t driver_text_len ) {
   
   char* so_path = md_fullpath( conf->data_root, MD_CLOSURE_TMPFILE_NAME, NULL );
   if( so_path == NULL ) {
      return -ENOMEM;
   }
   
   int rc = md_write_to_tmpfile( so_path, driver_text, driver_text_len, _so_path_ret );
   
   if( rc != 0 ) {
      SG_error("md_write_to_tmpfile(%s) rc = %d\n", so_path, rc );
   }
   
   SG_safe_free( so_path );
   return rc;
}

// read and link MS-supplied closure from a file on disk
// return 0 on success
// return -ENODATA on failure to open the .so file
// return -ENOENT if closure->ignore_stubs is false and we're missing a closure method
int md_load_driver( struct md_closure* closure, char const* so_path, struct md_closure_callback_entry* closure_symtable ) {
   
   closure->so_handle = dlopen( so_path, RTLD_LAZY );
   if ( closure->so_handle == NULL ) {
      
      SG_error( "dlopen error = %s\n", dlerror() );
      return -ENODATA;
   }
   
   // load each symbol into its respective address
   for( int i = 0; closure_symtable[i].sym_name != NULL; i++ ) {
      
      void* sym_ptr = dlsym( closure->so_handle, closure_symtable[i].sym_name );
      closure_symtable[i].sym_ptr = sym_ptr;
      
      if( closure_symtable[i].sym_ptr == NULL ) {
         
         if( closure->ignore_stubs ) {
            
            SG_warn("Unable to resolve method '%s', error = %s\n", closure_symtable[i].sym_name, dlerror() );
         }
         else {
            
            SG_error("dlsym(%s) error = %s\n", closure_symtable[i].sym_name, dlerror());
         
            dlclose( closure->so_handle );
            closure->so_handle = NULL;
            
            return -ENOENT;
         }
      }
      else {
         
         SG_debug("Loaded '%s' at %p\n", closure_symtable[i].sym_name, closure_symtable[i].sym_ptr );
      }
   }
   
   return 0;
}

// read-lock a closure
int md_closure_rlock( struct md_closure* closure ) {
   return pthread_rwlock_rdlock( &closure->reload_lock );
}


// write-lock a closure
int md_closure_wlock( struct md_closure* closure ) {
   return pthread_rwlock_wrlock( &closure->reload_lock );
}

// unlock a closure
int md_closure_unlock( struct md_closure* closure ) {
   return pthread_rwlock_unlock( &closure->reload_lock );
}


// reload the given closure's driver.  Shut it down, get the new code and state, and start it back up again.
// If we fail to load or initialize the new closure, then keep the old one around.
// if NULL is passed for driver_text, this method reloads from the closure's so_path member.
// if driver_text is not NULL, then this method reloads the closure from the text AND sets closure->on_disk to false, meaning that the stored data will be unlinked on shutdown.
// closure must be write-locked!
// return 0 on success, and reload the contents of *closure 
// return -ENOMEM on OOM 
// return -EINVAL if the closure does not have so_path set, and no driver text is given
// return -ENODATA on failure to store the driver text to a temporary file
// return non-zero if the new closure's driver failed to initialize
int md_closure_driver_reload( struct md_syndicate_conf* conf, struct md_closure* closure, char const* driver_text, size_t driver_text_len ) {
   int rc = 0;
   
   struct md_closure new_closure;
   
   memset( &new_closure, 0, sizeof(struct md_closure) );
   
   // preserve closure-handling preferences
   new_closure.ignore_stubs = closure->ignore_stubs;
   
   bool stored_to_disk = false;
   char* new_so_path = NULL;
   
   if( driver_text != NULL ) {
      
      // store to disk    
      rc = md_write_driver( conf, &new_so_path, driver_text, driver_text_len );
      if( rc != 0 && rc != -ENOENT ) {
         
         SG_error("md_write_driver rc = %d\n", rc);
         return -ENODATA;
      }
      
      stored_to_disk = true;
   }
   else if( closure->so_path != NULL ) {
      
      // reload from disk 
      new_so_path = SG_strdup_or_null( closure->so_path );
      if( new_so_path == NULL ) {
         
         return -ENOMEM;
      }
   }
   else {
      
      // invalid arguments
      return -EINVAL;
   }
   
   // there's closure code to be had...
   new_closure.callbacks = md_closure_callback_table_from_prototype( closure->callbacks );
   
   if( new_closure.callbacks == NULL ) {
      
      // OOM 
      if( stored_to_disk ) {
         unlink( new_so_path );
      }
      SG_safe_free( new_so_path );
      return -ENOMEM;
   }

   // shut down the existing closure
   void* shutdown_sym = md_closure_find_callback( closure, "closure_shutdown" );
   if( shutdown_sym ) {
      
      // NOTE: can't use our MD_CLOSURE_CALL method here, since it would deadlock
      md_closure_shutdown_func shutdown_cb = reinterpret_cast<md_closure_shutdown_func>( shutdown_sym );
      
      int closure_shutdown_rc = (*shutdown_cb)( closure->cls );
      
      if( closure_shutdown_rc != 0 ) {
         
         SG_warn("closure->shutdown rc = %d\n", closure_shutdown_rc );
      }
   }
   
   // load the driver
   rc = md_load_driver( &new_closure, new_so_path, new_closure.callbacks );
   if( rc != 0 ) {
      
      SG_error("closure_load(%s) rc = %d\n", new_so_path, rc );
      
      if( stored_to_disk ) {
         unlink( new_so_path );
      }
      
      SG_safe_free( new_so_path );
      return rc;
   }
   
   // success so far... initialize it 
   void* init_sym = md_closure_find_callback( &new_closure, "closure_init" );
   if( init_sym ) {
      
      // NOTE: can't use our MD_CLOSURE_CALL method here, since it would deadlock
      md_closure_init_func init_cb = reinterpret_cast<md_closure_init_func>( init_sym );
      
      int closure_init_rc = init_cb( &new_closure, &new_closure.cls );
      
      if( closure_init_rc != 0 ) {
         SG_error("closure->init() rc = %d\n", closure_init_rc );
      }
      
      if( stored_to_disk ) {
         unlink( new_so_path );
      }
      
      SG_safe_free( new_so_path );
      
      return closure_init_rc;
   }
   
   // successful initialization!   
   // copy over the dynamic link handle
   void* old_so_handle = closure->so_handle;
   closure->so_handle = new_closure.so_handle;
   
   if( old_so_handle != NULL ) {
      dlclose( old_so_handle );            
   }
   
   // load the new callbacks
   struct md_closure_callback_entry* old_callbacks = closure->callbacks;
   closure->callbacks = new_closure.callbacks;
   
   if( old_callbacks != NULL ) {
      md_closure_callback_table_free( old_callbacks );
      SG_safe_free( old_callbacks );
   }
   
   // clean up old cached closure code
   if( closure->so_path ) {
      
      if( stored_to_disk ) {
         unlink( closure->so_path );
         
         if( closure->on_disk ) {
            SG_warn("Replaced '%s' with caller-supplied code\n", closure->so_path );
         }
         
         closure->on_disk = false;
      }
      
      SG_safe_free( closure->so_path );
   }
   
   closure->so_path = new_so_path;
   
   return rc;
}


// reload the closure 
// return 0 on success 
// return -ENOMEM on OOM 
// return -EINVAL on failure to parse
int md_closure_reload( struct md_closure* closure, struct md_syndicate_conf* conf, EVP_PKEY* pubkey, EVP_PKEY* privkey, char const* closure_text, size_t closure_text_len ) {
   
   md_closure_wlock( closure );
   
   // attempt to reload the essentials...
   md_closure_conf_t* closure_conf = SG_safe_new( md_closure_conf_t() );
   md_closure_secrets_t* closure_secrets = SG_safe_new( md_closure_secrets_t() );
   
   char* driver_bin = NULL;
   size_t driver_bin_len = 0;
   
   int rc = md_parse_closure( closure_conf, pubkey, privkey, closure_secrets, &driver_bin, &driver_bin_len, closure_text, closure_text_len );
   if( rc != 0 ) {
      
      SG_error("md_parse_closure rc = %d\n", rc );
      
      SG_safe_delete( closure_conf );
      SG_safe_delete( closure_secrets );
      
      md_closure_unlock( closure );
      return rc;
   }
   
   // copy over the new conf and secrets...
   md_closure_conf_t* old_closure_conf = closure->closure_conf;
   md_closure_secrets_t* old_closure_secrets = closure->closure_secrets;
   
   closure->closure_conf = closure_conf;
   closure->closure_secrets = closure_secrets;
   
   // attempt to reload the driver...
   rc = md_closure_driver_reload( conf, closure, driver_bin, driver_bin_len );
   if( rc != 0 ) {
      
      SG_error("md_closure_driver_reload rc = %d\n", rc );
      
      // revert 
      closure->closure_conf = old_closure_conf;
      closure->closure_secrets = old_closure_secrets;
      
      SG_safe_delete( closure_conf );
      SG_safe_delete( closure_secrets );
   }
   else {
      
      // success!
      SG_safe_delete( old_closure_conf );
      SG_safe_delete( old_closure_secrets );
   }
   
   md_closure_unlock( closure );
   
   SG_safe_free( driver_bin );
   
   return rc;
}

// shut down the closure 
// alwyas succeeds
int md_closure_shutdown( struct md_closure* closure ) {
   
   // call the closure shutdown...
   int rc = 0;
   
   md_closure_wlock( closure );
   
   closure->running = 0;
   
   // closure shutdown method?
   void* shutdown_sym = md_closure_find_callback( closure, "closure_shutdown" );
   if( shutdown_sym ) {
      
      // NOTE: can't use our MD_CLOSURE_CALL method here, since it would deadlock
      md_closure_shutdown_func shutdown_cb = reinterpret_cast<md_closure_shutdown_func>( shutdown_sym );
      
      int closure_shutdown_rc = shutdown_cb( closure->cls );
      
      if( closure_shutdown_rc != 0 ) {
         SG_warn("closure->shutdown rc = %d\n", closure_shutdown_rc );
      }
   }
   
   if( closure->so_path != NULL ) {
      
      if( !closure->on_disk ) {
         unlink( closure->so_path );
      }
      
      SG_safe_free( closure->so_path );
   }
   
   if( closure->so_handle != NULL ) {
      
      dlclose( closure->so_handle );
      closure->so_handle = NULL;
   }
   
   if( closure->callbacks != NULL ) {
      
      md_closure_callback_table_free( closure->callbacks );
      SG_safe_free( closure->callbacks );
   }
   
   SG_safe_delete( closure->closure_conf );
   SG_safe_delete( closure->closure_secrets );
   
   md_closure_unlock( closure );
   pthread_rwlock_destroy( &closure->reload_lock );
 
   return rc;
}

// look up a callback 
// return a non-NULL pointer to it on success
// return NULL if not found, or if the closure is not initialized
void* md_closure_find_callback( struct md_closure* closure, char const* cb_name ) {
   
   if( closure == NULL ) {
      return NULL;
   }
   
   if( closure->running == 0 ) {
      return NULL;
   }
   
   if( closure->callbacks == NULL ) {
      return NULL;
   }
   
   md_closure_rlock( closure );
   
   void* ret = NULL;
   
   for( int i = 0; closure->callbacks[i].sym_name != NULL; i++ ) {
      
      if( strcmp( cb_name, closure->callbacks[i].sym_name ) == 0 ) {
         
         ret = closure->callbacks[i].sym_ptr;
      }
   }
   
   md_closure_unlock( closure );
   
   return ret;
}

// get a config value 
// return 0 on success, and put the value and length into *value and *len (*value will be malloc'ed)
// return -ENONET if no such config key exists 
// return -ENOMEM if OOM
int md_closure_get_config( struct md_closure* closure, char const* key, char** value, size_t* len ) {
   
   md_closure_rlock( closure );
   
   if( closure->closure_conf == NULL ) {
      
      md_closure_unlock( closure );
      return -ENOENT;
   }
   
   int rc = 0;
   
   try {
      md_closure_conf_t::iterator itr = closure->closure_conf->find( string(key) );
      
      if( itr != closure->closure_conf->end() ) {
         
         size_t ret_len = itr->second.size() + 1;
         
         char* ret = SG_CALLOC( char, ret_len );
         if( ret == NULL ) {
            
            rc = -ENOMEM;
         }
         else {
            memcpy( ret, itr->second.data(), ret_len );
            
            *value = ret;
            *len = ret_len;
         }
      }
      else {
         rc = -ENOENT;
      }
   }
   catch( bad_alloc& ba ) {
      
      rc = -ENOMEM;
   }
   
   md_closure_unlock( closure );
   return rc;
}

// get a secret value 
// return 0 on success, and put the value and length in *value and *len (*value will be malloc'ed)
// return -ENOENT if there is no such secret
// return -ENOMEM if OOM
int md_closure_get_secret( struct md_closure* closure, char const* key, char** value, size_t* len ) {
   
   md_closure_rlock( closure );
   
   if( closure->closure_secrets == NULL ) {
      md_closure_unlock( closure );
      return -ENOENT;
   }
   
   int rc = 0;
   
   try {
      md_closure_conf_t::iterator itr = closure->closure_secrets->find( string(key) );
      
      if( itr != closure->closure_secrets->end() ) {
         
         size_t ret_len = itr->second.size() + 1;
         char* ret = SG_CALLOC( char, ret_len );
         
         if( ret == NULL ) { 
            
            rc = -ENOMEM;
         }
         else {
            memcpy( ret, itr->second.data(), ret_len );
            
            *value = ret;
            *len = ret_len;
         }
      }
      else {
         rc = -ENOENT;
      }
   }
   catch( bad_alloc& ba ) {
      rc = -ENOMEM;
   }
   
   md_closure_unlock( closure );
   return rc;
}
