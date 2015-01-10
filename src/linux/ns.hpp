/**
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __LINUX_NS_HPP__
#define __LINUX_NS_HPP__

// This file contains Linux-only OS utilities.
#ifndef __linux__
#error "linux/ns.hpp is only available on Linux systems."
#endif

#include <sched.h>
#include <unistd.h>

#include <sys/syscall.h>

#include <set>
#include <string>

#include <stout/error.hpp>
#include <stout/hashmap.hpp>
#include <stout/nothing.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/proc.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>

#include <stout/os/exists.hpp>
#include <stout/os/ls.hpp>

#include <process/collect.hpp>
#include <process/future.hpp>
#include <process/reap.hpp>

namespace ns {

// Returns all the supported namespaces by the kernel.
inline std::set<std::string> namespaces()
{
  std::set<std::string> result;
  Try<std::list<std::string> > entries = os::ls("/proc/self/ns");
  if (entries.isSome()) {
    foreach (const std::string& entry, entries.get()) {
      result.insert(entry);
    }
  }
  return result;
}


// Returns the nstype (e.g., CLONE_NEWNET, CLONE_NEWNS, etc.) for the
// given namespace which will be used when calling ::setns.
inline Try<int> nstype(const std::string& ns)
{
  hashmap<std::string, int> nstypes;

#ifdef CLONE_NEWNS
  nstypes["mnt"] = CLONE_NEWNS;
#else
  nstypes["mnt"] = 0x00020000;
#endif

#ifdef CLONE_NEWUTS
  nstypes["uts"] = CLONE_NEWUTS;
#else
  nstypes["uts"] = 0x04000000;
#endif

#ifdef CLONE_NEWIPC
  nstypes["ipc"] = CLONE_NEWIPC;
#else
  nstypes["ipc"] = 0x08000000;
#endif

#ifdef CLONE_NEWNET
  nstypes["net"] = CLONE_NEWNET;
#else
  nstypes["net"] = 0x40000000;
#endif

#ifdef CLONE_NEWUSER
  nstypes["user"] = CLONE_NEWUSER;
#else
  nstypes["user"] = 0x10000000;
#endif

#ifdef CLONE_NEWPID
  nstypes["pid"] = CLONE_NEWPID;
#else
  nstypes["pid"] = 0x20000000;
#endif

  if (!nstypes.contains(ns)) {
    return Error("Unknown namespace '" + ns + "'");
  }

  return nstypes[ns];
}


// Re-associate the calling process with the specified namespace. The
// path refers to one of the corresponding namespace entries in the
// /proc/[pid]/ns/ directory (or bind mounted elsewhere). We do not
// allow a process with multiple threads to call this function because
// it will lead to some weird situations where different threads of a
// process are in different namespaces.
inline Try<Nothing> setns(const std::string& path, const std::string& ns)
{
  // Return error if there're multiple threads in the calling process.
  Try<std::set<pid_t> > threads = proc::threads(::getpid());
  if (threads.isError()) {
    return Error(
        "Failed to get the threads of the current process: " +
        threads.error());
  } else if (threads.get().size() > 1) {
    return Error("Multiple threads exist in the current process");
  }

  if (ns::namespaces().count(ns) == 0) {
    return Error("Namespace '" + ns + "' is not supported");
  }

  // Currently, we don't support pid namespace as its semantics is
  // different from other namespaces (instead of re-associating the
  // calling thread, it re-associates the *children* of the calling
  // thread with the specified namespace).
  if (ns == "pid") {
    return Error("Pid namespace is not supported");
  }

#ifdef O_CLOEXEC
  Try<int> fd = os::open(path, O_RDONLY | O_CLOEXEC);
#else
  Try<int> fd = os::open(path, O_RDONLY);
#endif

  if (fd.isError()) {
    return Error("Failed to open '" + path + "': " + fd.error());
  }

#ifndef O_CLOEXEC
  Try<Nothing> cloexec = os::cloexec(fd.get());
  if (cloexec.isError()) {
    os::close(fd.get());
    return Error("Failed to cloexec: " + cloexec.error());
  }
#endif

  Try<int> nstype = ns::nstype(ns);
  if (nstype.isError()) {
    return Error(nstype.error());
  }

#ifdef SYS_setns
  int ret = ::syscall(SYS_setns, fd.get(), nstype.get());
#elif __x86_64__
  // A workaround for those hosts that have an old glibc (older than
  // 2.14) but have a new kernel. The magic number '308' here is the
  // syscall number for 'setns' on x86_64 architecture.
  int ret = ::syscall(308, fd.get(), nstype.get());
#else
#error "setns is not available"
#endif

  if (ret == -1) {
    // Save the errno as it might be overwritten by 'os::close' below.
    ErrnoError error;
    os::close(fd.get());
    return error;
  }

  os::close(fd.get());
  return Nothing();
}


// Re-associate the calling process with the specified namespace. The
// pid specifies the process whose namespace we will associate.
inline Try<Nothing> setns(pid_t pid, const std::string& ns)
{
  if (!os::exists(pid)) {
    return Error("Pid " + stringify(pid) + " does not exist");
  }

  std::string path = path::join("/proc", stringify(pid), "ns", ns);
  if (!os::exists(path)) {
    return Error("Namespace '" + ns + "' is not supported");
  }

  return ns::setns(path, ns);
}


// Get the inode number of the specified namespace for the specified
// pid. The inode number identifies the namespace and can be used for
// comparisons, i.e., two processes with the same inode for a given
// namespace type are in the same namespace.
inline Try<ino_t> getns(pid_t pid, const std::string& ns)
{
  if (!os::exists(pid)) {
    return Error("Pid " + stringify(pid) + " does not exist");
  }

  if (ns::namespaces().count(ns) < 1) {
    return Error("Namespace '" + ns + "' is not supported");
  }

  std::string path = path::join("/proc", stringify(pid), "ns", ns);
  struct stat s;
  if (::stat(path.c_str(), &s) < 0) {
    return ErrnoError("Failed to stat " + ns + " namespace handle"
                      " for pid " + stringify(pid));
  }

  return s.st_ino;
}


namespace pid {

namespace internal {

inline Nothing _nothing() { return Nothing(); }

} // namespace internal {

inline process::Future<Nothing> destroy(ino_t inode)
{
  // Check we're not trying to kill the root namespace.
  Try<ino_t> ns = ns::getns(1, "pid");
  if (ns.isError()) {
    return process::Failure(ns.error());
  }

  if (ns.get() == inode) {
    return process::Failure("Cannot destroy root pid namespace");
  }

  // Or ourselves.
  ns = ns::getns(::getpid(), "pid");
  if (ns.isError()) {
    return process::Failure(ns.error());
  }

  if (ns.get() == inode) {
    return process::Failure("Cannot destroy own pid namespace");
  }

  // Signal all pids in the namespace, including the init pid if it's
  // still running. Once the init pid has been signalled the kernel
  // will prevent any new children forking in the namespace and will
  // also signal all other pids in the namespace.
  Try<std::set<pid_t>> pids = os::pids();
  if (pids.isError()) {
    return process::Failure("Failed to list of processes");
  }

  foreach (pid_t pid, pids.get()) {
    // Ignore any errors, probably because the process no longer
    // exists, and ignorable otherwise.
    Try<ino_t> ns = ns::getns(pid, "pid");
    if (ns.isSome() && ns.get() == inode) {
      kill(pid, SIGKILL);
    }
  }

  // Get a new snapshot and do a second pass of the pids to capture
  // any pids that are dying so we can reap them.
  pids = os::pids();
  if (pids.isError()) {
    return process::Failure("Failed to list of processes");
  }

  std::list<process::Future<Option<int>>> futures;

  foreach (pid_t pid, pids.get()) {
    Try<ino_t> ns = ns::getns(pid, "pid");
    if (ns.isSome() && ns.get() == inode) {
      futures.push_back(process::reap(pid));
    }

    // Ignore any errors, probably because the process no longer
    // exists, and ignorable otherwise.
  }

  // Wait for all the signalled processes to terminate. The pid
  // namespace wil then be empty and will be released by the kernel
  // (unless there are additional references).
  return process::collect(futures)
    .then(lambda::bind(&internal::_nothing));
}

} // namespace pid {
} // namespace ns {

#endif // __LINUX_NS_HPP__
