/*  vim:expandtab:shiftwidth=2:tabstop=2:smarttab:
 * 
 *  uTest, libtest
 *
 *  Copyright (C) 2011 Data Differential, http://datadifferential.com/
 *  Copyright (C) 2006-2009 Brian Aker
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are
 *  met:
 *
 *      * Redistributions of source code must retain the above copyright
 *  notice, this list of conditions and the following disclaimer.
 *
 *      * Redistributions in binary form must reproduce the above
 *  copyright notice, this list of conditions and the following disclaimer
 *  in the documentation and/or other materials provided with the
 *  distribution.
 *
 *      * The names of its contributors may not be used to endorse or
 *  promote products derived from this software without specific prior
 *  written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <libtest/common.h>

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <ctime>
#include <fnmatch.h>
#include <iostream>

#include <signal.h>

#include <libtest/stats.h>
#include <libtest/signal.h>

#ifndef __INTEL_COMPILER
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif

static in_port_t global_port= 0;
static char global_socket[1024];

in_port_t default_port()
{
  return global_port;
}
 
void set_default_port(in_port_t port)
{
  global_port= port;
}

const char *default_socket()
{
  assert(global_socket[0]);
  return global_socket;
}

bool test_is_local()
{
  return (getenv("LIBTEST_LOCAL"));
}

void set_default_socket(const char *socket)
{
  strncpy(global_socket, socket, strlen(socket));
}

static void stats_print(Stats *stats)
{
  std::cout << "\tTotal Collections\t\t\t\t" << stats->collection_total << std::endl;
  std::cout << "\tFailed Collections\t\t\t\t" << stats->collection_failed << std::endl;
  std::cout << "\tSkipped Collections\t\t\t\t" << stats->collection_skipped << std::endl;
  std::cout << "\tSucceeded Collections\t\t\t\t" << stats->collection_success << std::endl;
  std::cout << std::endl;
  std::cout << "Total\t\t\t\t" << stats->total << std::endl;
  std::cout << "\tFailed\t\t\t" << stats->failed << std::endl;
  std::cout << "\tSkipped\t\t\t" << stats->skipped << std::endl;
  std::cout << "\tSucceeded\t\t" << stats->success << std::endl;
}

static long int timedif(struct timeval a, struct timeval b)
{
  long us, s;

  us = (long)(a.tv_usec - b.tv_usec);
  us /= 1000;
  s = (long)(a.tv_sec - b.tv_sec);
  s *= 1000;
  return s + us;
}

const char *test_strerror(test_return_t code)
{
  switch (code) {
  case TEST_SUCCESS:
    return "ok";

  case TEST_FAILURE:
    return "failed";

  case TEST_MEMORY_ALLOCATION_FAILURE:
    return "memory allocation";

  case TEST_SKIPPED:
    return "skipped";

  case TEST_FATAL:
    break;
  }

  return "failed";
}

void create_core(void)
{
  if (getenv("LIBMEMCACHED_NO_COREDUMP") == NULL)
  {
    pid_t pid= fork();

    if (pid == 0)
    {
      abort();
    }
    else
    {
      while (waitpid(pid, NULL, 0) != pid) {};
    }
  }
}

static Framework *world= NULL;
int main(int argc, char *argv[])
{
  world= new Framework();

  if (not world)
  {
    return EXIT_FAILURE;
  }

  setup_signals();

  Stats stats;

  get_world(world);

  test_return_t error;
  void *creators_ptr= world->create(error);
  if (test_failed(error))
  {
    Error << "create() failed";
    return EXIT_FAILURE;
  }

  char *collection_to_run= NULL;
  if (argc > 1)
  {
    collection_to_run= argv[1];
  }
  else if (getenv("TEST_COLLECTION"))
  {
    collection_to_run= getenv("TEST_COLLECTION");
  }

  if (collection_to_run)
  {
    std::cout << "Only testing " <<  collection_to_run << std::endl;
  }

  char *wildcard= NULL;
  if (argc == 3)
  {
    wildcard= argv[2];
  }

  for (collection_st *next= world->collections; next->name and (not is_shutdown()); next++)
  {
    test_return_t collection_rc= TEST_SUCCESS;
    bool failed= false;
    bool skipped= false;

    if (collection_to_run && fnmatch(collection_to_run, next->name, 0))
      continue;

    stats.collection_total++;

    collection_rc= world->startup(creators_ptr);

    if (collection_rc == TEST_SUCCESS and next->pre)
    {
      collection_rc= world->runner->pre(next->pre, creators_ptr);
    }

    switch (collection_rc)
    {
    case TEST_SUCCESS:
      break;

    case TEST_FATAL:
    case TEST_FAILURE:
      Error << next->name << " [ failed ]";
      stats.collection_failed++;
      set_shutdown(SHUTDOWN_GRACEFUL);
      goto cleanup;

    case TEST_SKIPPED:
      Log << next->name << " [ skipping ]";
      stats.collection_skipped++;
      goto cleanup;

    case TEST_MEMORY_ALLOCATION_FAILURE:
      test_assert(0, "Allocation failure, or unknown return");
    }

    Log << "Collection: " << next->name;

    for (test_st *run= next->tests; run->name; run++)
    {
      struct timeval start_time, end_time;
      long int load_time= 0;

      if (wildcard && fnmatch(wildcard, run->name, 0))
      {
        continue;
      }

      test_return_t return_code;
      if (test_success(return_code= world->item.startup(creators_ptr)))
      {
        if (test_success(return_code= world->item.flush(creators_ptr, run)))
        {
          // @note pre will fail is SKIPPED is returned
          if (test_success(return_code= world->item.pre(creators_ptr)))
          {
            { // Runner Code
              gettimeofday(&start_time, NULL);
              return_code= world->runner->run(run->test_fn, creators_ptr);
              gettimeofday(&end_time, NULL);
              load_time= timedif(end_time, start_time);
            }
          }

          // @todo do something if post fails
          (void)world->item.post(creators_ptr);
        }
        else if (return_code == TEST_SKIPPED)
        { }
        else if (return_code == TEST_FAILURE)
        {
          Error << " item.flush(failure)";
          set_shutdown(SHUTDOWN_GRACEFUL);
        }
      }
      else if (return_code == TEST_SKIPPED)
      { }
      else if (return_code == TEST_FAILURE)
      {
        Error << " item.startup(failure)";
        set_shutdown(SHUTDOWN_GRACEFUL);
      }

      stats.total++;

      switch (return_code)
      {
      case TEST_SUCCESS:
        Log << "\tTesting " << run->name <<  "\t\t\t\t\t" << load_time / 1000 << "." << load_time % 1000 << "[ " << test_strerror(return_code) << " ]";
        stats.success++;
        break;

      case TEST_FATAL:
      case TEST_FAILURE:
        stats.failed++;
        failed= true;
        Log << "\tTesting " << run->name <<  "\t\t\t\t\t" << "[ " << test_strerror(return_code) << " ]";
        break;

      case TEST_SKIPPED:
        stats.skipped++;
        skipped= true;
        Log << "\tTesting " << run->name <<  "\t\t\t\t\t" << "[ " << test_strerror(return_code) << " ]";
        break;

      case TEST_MEMORY_ALLOCATION_FAILURE:
        test_assert(0, "Memory Allocation Error");
      }

      if (test_failed(world->on_error(return_code, creators_ptr)))
      {
        Error << "Failed while running on_error()";
        set_shutdown(SHUTDOWN_GRACEFUL);
        break;
      }
    }

    if (next->post and world->runner->post)
    {
      (void) world->runner->post(next->post, creators_ptr);
    }

    if (failed == 0 and skipped == 0)
    {
      stats.collection_success++;
    }
cleanup:

    world->shutdown(creators_ptr);
    Logn();
  }

  if (not is_shutdown())
  {
    set_shutdown(SHUTDOWN_GRACEFUL);
  }

  int exit_code= EXIT_SUCCESS;
  shutdown_t status= get_shutdown();
  if (status == SHUTDOWN_FORCED)
  {
    Log << "Tests were aborted.";
    exit_code= EXIT_FAILURE;
  }
  else if (stats.collection_failed)
  {
    Log << "Some test failed.";
    exit_code= EXIT_FAILURE;
  }
  else if (stats.collection_skipped)
  {
    Log << "Some tests were skipped.";
  }
  else
  {
    Log << "All tests completed successfully.";
  }

  stats_print(&stats);

  delete world;

  return exit_code;
}
