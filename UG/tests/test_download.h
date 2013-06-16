/*
   Copyright 2011 Jude Nelson

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

#ifndef _TEST_DOWNLOAD_H_
#define _TEST_DOWNLOAD_H_

#include <limits.h>

#include "download.h"
#include "libsyndicate.h"

#define NUM_THREADS 10

struct test_thread_args { 
   int id;
   struct download* dh;
};

#define RAM_CUTOFF 10000
#define DISK_CUTOFF RAM_CUTOFF * 10

#endif