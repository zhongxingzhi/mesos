#include <string>

#include <boost/shared_array.hpp>

#include <process/future.hpp>
#include <process/io.hpp>
#include <process/process.hpp> // For process::initialize.

#include <stout/lambda.hpp>
#include <stout/memory.hpp>
#include <stout/nothing.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>

using std::string;

namespace process {
namespace io {
namespace internal {

void read(
    int fd,
    void* data,
    size_t size,
    const memory::shared_ptr<Promise<size_t> >& promise,
    const Future<short>& future)
{
  // Ignore this function if the read operation has been discarded.
  if (promise->future().hasDiscard()) {
    CHECK(!future.isPending());
    promise->discard();
    return;
  }

  if (size == 0) {
    promise->set(0);
    return;
  }

  if (future.isDiscarded()) {
    promise->fail("Failed to poll: discarded future");
  } else if (future.isFailed()) {
    promise->fail(future.failure());
  } else {
    ssize_t length = ::read(fd, data, size);
    if (length < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        // Restart the read operation.
        Future<short> future =
          io::poll(fd, process::io::READ).onAny(
              lambda::bind(&internal::read,
                           fd,
                           data,
                           size,
                           promise,
                           lambda::_1));

        // Stop polling if a discard occurs on our future.
        promise->future().onDiscard(
            lambda::bind(&process::internal::discard<short>,
                         WeakFuture<short>(future)));
      } else {
        // Error occurred.
        promise->fail(strerror(errno));
      }
    } else {
      promise->set(length);
    }
  }
}


void write(
    int fd,
    void* data,
    size_t size,
    const memory::shared_ptr<Promise<size_t> >& promise,
    const Future<short>& future)
{
  // Ignore this function if the write operation has been discarded.
  if (promise->future().hasDiscard()) {
    promise->discard();
    return;
  }

  if (size == 0) {
    promise->set(0);
    return;
  }

  if (future.isDiscarded()) {
    promise->fail("Failed to poll: discarded future");
  } else if (future.isFailed()) {
    promise->fail(future.failure());
  } else {
    // Do a write but ignore SIGPIPE so we can return an error when
    // writing to a pipe or socket where the reading end is closed.
    // TODO(benh): The 'suppress' macro failed to work on OS X as it
    // appears that signal delivery was happening asynchronously.
    // That is, the signal would not appear to be pending when the
    // 'suppress' block was closed thus the destructor for
    // 'Suppressor' was not waiting/removing the signal via 'sigwait'.
    // It also appeared that the signal would be delivered to another
    // thread even if it remained blocked in this thiread. The
    // workaround here is to check explicitly for EPIPE and then do
    // 'sigwait' regardless of what 'os::signals::pending' returns. We
    // don't have that luxury with 'Suppressor' and arbitrary signals
    // because we don't always have something like EPIPE to tell us
    // that a signal is (or will soon be) pending.
    bool pending = os::signals::pending(SIGPIPE);
    bool unblock = !pending ? os::signals::block(SIGPIPE) : false;

    ssize_t length = ::write(fd, data, size);

    // Save the errno so we can restore it after doing sig* functions
    // below.
    int errno_ = errno;

    if (length < 0 && errno == EPIPE && !pending) {
      sigset_t mask;
      sigemptyset(&mask);
      sigaddset(&mask, SIGPIPE);

      int result;
      do {
        int ignored;
        result = sigwait(&mask, &ignored);
      } while (result == -1 && errno == EINTR);
    }

    if (unblock) {
      os::signals::unblock(SIGPIPE);
    }

    errno = errno_;

    if (length < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        // Restart the write operation.
        Future<short> future =
          io::poll(fd, process::io::WRITE).onAny(
              lambda::bind(&internal::write,
                           fd,
                           data,
                           size,
                           promise,
                           lambda::_1));

        // Stop polling if a discard occurs on our future.
        promise->future().onDiscard(
            lambda::bind(&process::internal::discard<short>,
                         WeakFuture<short>(future)));
      } else {
        // Error occurred.
        promise->fail(strerror(errno));
      }
    } else {
      // TODO(benh): Retry if 'length' is 0?
      promise->set(length);
    }
  }
}

} // namespace internal {


Future<size_t> read(int fd, void* data, size_t size)
{
  process::initialize();

  memory::shared_ptr<Promise<size_t> > promise(new Promise<size_t>());

  // Check the file descriptor.
  Try<bool> nonblock = os::isNonblock(fd);
  if (nonblock.isError()) {
    // The file descriptor is not valid (e.g., has been closed).
    promise->fail(
        "Failed to check if file descriptor was non-blocking: " +
        nonblock.error());
    return promise->future();
  } else if (!nonblock.get()) {
    // The file descriptor is not non-blocking.
    promise->fail("Expected a non-blocking file descriptor");
    return promise->future();
  }

  // Because the file descriptor is non-blocking, we call read()
  // immediately. The read may in turn call poll if necessary,
  // avoiding unnecessary polling. We also observed that for some
  // combination of libev and Linux kernel versions, the poll would
  // block for non-deterministically long periods of time. This may be
  // fixed in a newer version of libev (we use 3.8 at the time of
  // writing this comment).
  internal::read(fd, data, size, promise, io::READ);

  return promise->future();
}


Future<size_t> write(int fd, void* data, size_t size)
{
  process::initialize();

  memory::shared_ptr<Promise<size_t> > promise(new Promise<size_t>());

  // Check the file descriptor.
  Try<bool> nonblock = os::isNonblock(fd);
  if (nonblock.isError()) {
    // The file descriptor is not valid (e.g., has been closed).
    promise->fail(
        "Failed to check if file descriptor was non-blocking: " +
        nonblock.error());
    return promise->future();
  } else if (!nonblock.get()) {
    // The file descriptor is not non-blocking.
    promise->fail("Expected a non-blocking file descriptor");
    return promise->future();
  }

  // Because the file descriptor is non-blocking, we call write()
  // immediately. The write may in turn call poll if necessary,
  // avoiding unnecessary polling. We also observed that for some
  // combination of libev and Linux kernel versions, the poll would
  // block for non-deterministically long periods of time. This may be
  // fixed in a newer version of libev (we use 3.8 at the time of
  // writing this comment).
  internal::write(fd, data, size, promise, io::WRITE);

  return promise->future();
}


namespace internal {

#if __cplusplus >= 201103L
Future<string> _read(
    int fd,
    const memory::shared_ptr<string>& buffer,
    const boost::shared_array<char>& data,
    size_t length)
{
  return io::read(fd, data.get(), length)
    .then([=] (size_t size) -> Future<string> {
      if (size == 0) { // EOF.
        return string(*buffer);
      }
      buffer->append(data.get(), size);
      return _read(fd, buffer, data, length);
    });
}
#else
// Forward declataion.
Future<string> _read(
    int fd,
    const memory::shared_ptr<string>& buffer,
    const boost::shared_array<char>& data,
    size_t length);


Future<string> __read(
    size_t size,
    int fd,
    const memory::shared_ptr<string>& buffer,
    const boost::shared_array<char>& data,
    size_t length)
{
  if (size == 0) { // EOF.
    return string(*buffer);
  }

  buffer->append(data.get(), size);

  return _read(fd, buffer, data, length);
}


Future<string> _read(
    int fd,
    const memory::shared_ptr<string>& buffer,
    const boost::shared_array<char>& data,
    size_t length)
{
  return io::read(fd, data.get(), length)
    .then(lambda::bind(&__read, lambda::_1, fd, buffer, data, length));
}
#endif // __cplusplus >= 201103L


#if __cplusplus >= 201103L
Future<Nothing> _write(
    int fd,
    Owned<string> data,
    size_t index)
{
  return io::write(fd, (void*) (data->data() + index), data->size() - index)
    .then([=] (size_t length) -> Future<Nothing> {
      if (index + length == data->size()) {
        return Nothing();
      }
      return _write(fd, data, index + length);
    });
}
#else
// Forward declaration.
Future<Nothing> _write(
    int fd,
    Owned<string> data,
    size_t index);


Future<Nothing> __write(
    int fd,
    Owned<string> data,
    size_t index,
    size_t length)
{
  if (index + length == data->size()) {
    return Nothing();
  }
  return _write(fd, data, index + length);
}


Future<Nothing> _write(
    int fd,
    Owned<string> data,
    size_t index)
{
  return io::write(fd, (void*) (data->data() + index), data->size() - index)
    .then(lambda::bind(&__write, fd, data, index, lambda::_1));
}
#endif // __cplusplus >= 201103L


#if __cplusplus >= 201103L
void _splice(
    int from,
    int to,
    size_t chunk,
    boost::shared_array<char> data,
    memory::shared_ptr<Promise<Nothing>> promise)
{
  // Stop splicing if a discard occured on our future.
  if (promise->future().hasDiscard()) {
    // TODO(benh): Consider returning the number of bytes already
    // spliced on discarded, or a failure. Same for the 'onDiscarded'
    // callbacks below.
    promise->discard();
    return;
  }

  // Note that only one of io::read or io::write is outstanding at any
  // one point in time thus the reuse of 'data' for both operations.

  Future<size_t> read = io::read(from, data.get(), chunk);

  // Stop reading (or potentially indefinitely polling) if a discard
  // occcurs on our future.
  promise->future().onDiscard(
      lambda::bind(&process::internal::discard<size_t>,
                   WeakFuture<size_t>(read)));

  read
    .onReady([=] (size_t size) {
      if (size == 0) { // EOF.
        promise->set(Nothing());
      } else {
        // Note that we always try and complete the write, even if a
        // discard has occured on our future, in order to provide
        // semantics where everything read is written. The promise
        // will eventually be discarded in the next read.
        io::write(to, string(data.get(), size))
          .onReady([=] () { _splice(from, to, chunk, data, promise); })
          .onFailed([=] (const string& message) { promise->fail(message); })
          .onDiscarded([=] () { promise->discard(); });
      }
    })
    .onFailed([=] (const string& message) { promise->fail(message); })
    .onDiscarded([=] () { promise->discard(); });
}
#else
// Forward declarations.
void __splice(
    int from,
    int to,
    size_t chunk,
    boost::shared_array<char> data,
    memory::shared_ptr<Promise<Nothing> > promise,
    size_t size);

void ___splice(
    memory::shared_ptr<Promise<Nothing> > promise,
    const string& message);

void ____splice(
    memory::shared_ptr<Promise<Nothing> > promise);


void _splice(
    int from,
    int to,
    size_t chunk,
    boost::shared_array<char> data,
    memory::shared_ptr<Promise<Nothing> > promise)
{
  // Stop splicing if a discard occured on our future.
  if (promise->future().hasDiscard()) {
    // TODO(benh): Consider returning the number of bytes already
    // spliced on discarded, or a failure. Same for the 'onDiscarded'
    // callbacks below.
    promise->discard();
    return;
  }

  Future<size_t> read = io::read(from, data.get(), chunk);

  // Stop reading (or potentially indefinitely polling) if a discard
  // occurs on our future.
  promise->future().onDiscard(
      lambda::bind(&process::internal::discard<size_t>,
                   WeakFuture<size_t>(read)));

  read
    .onReady(
        lambda::bind(&__splice, from, to, chunk, data, promise, lambda::_1))
    .onFailed(lambda::bind(&___splice, promise, lambda::_1))
    .onDiscarded(lambda::bind(&____splice, promise));
}


void __splice(
    int from,
    int to,
    size_t chunk,
    boost::shared_array<char> data,
    memory::shared_ptr<Promise<Nothing> > promise,
    size_t size)
{
  if (size == 0) { // EOF.
    promise->set(Nothing());
  } else {
    // Note that we always try and complete the write, even if a
    // discard has occured on our future, in order to provide
    // semantics where everything read is written. The promise will
    // eventually be discarded in the next read.
    io::write(to, string(data.get(), size))
      .onReady(lambda::bind(&_splice, from, to, chunk, data, promise))
      .onFailed(lambda::bind(&___splice, promise, lambda::_1))
      .onDiscarded(lambda::bind(&____splice, promise));
  }
}


void ___splice(
    memory::shared_ptr<Promise<Nothing> > promise,
    const string& message)
{
  promise->fail(message);
}


void ____splice(
    memory::shared_ptr<Promise<Nothing> > promise)
{
  promise->discard();
}
#endif // __cplusplus >= 201103L


Future<Nothing> splice(int from, int to, size_t chunk)
{
  boost::shared_array<char> data(new char[chunk]);

  // Rather than having internal::_splice return a future and
  // implementing internal::_splice as a chain of io::read and
  // io::write calls, we use an explicit promise that we pass around
  // so that we don't increase memory usage the longer that we splice.
  memory::shared_ptr<Promise<Nothing> > promise(new Promise<Nothing>());

  Future<Nothing> future = promise->future();

  _splice(from, to, chunk, data, promise);

  return future;
}

} // namespace internal {


Future<string> read(int fd)
{
  process::initialize();

  // Get our own copy of the file descriptor so that we're in control
  // of the lifetime and don't crash if/when someone by accidently
  // closes the file descriptor before discarding this future. We can
  // also make sure it's non-blocking and will close-on-exec. Start by
  // checking we've got a "valid" file descriptor before dup'ing.
  if (fd < 0) {
    return Failure(strerror(EBADF));
  }

  fd = dup(fd);
  if (fd == -1) {
    return Failure(ErrnoError("Failed to duplicate file descriptor"));
  }

  // Set the close-on-exec flag.
  Try<Nothing> cloexec = os::cloexec(fd);
  if (cloexec.isError()) {
    os::close(fd);
    return Failure(
        "Failed to set close-on-exec on duplicated file descriptor: " +
        cloexec.error());
  }

  // Make the file descriptor is non-blocking.
  Try<Nothing> nonblock = os::nonblock(fd);
  if (nonblock.isError()) {
    os::close(fd);
    return Failure(
        "Failed to make duplicated file descriptor non-blocking: " +
        nonblock.error());
  }

  // TODO(benh): Wrap up this data as a struct, use 'Owner'.
  // TODO(bmahler): For efficiency, use a rope for the buffer.
  memory::shared_ptr<string> buffer(new string());
  boost::shared_array<char> data(new char[BUFFERED_READ_SIZE]);

  return internal::_read(fd, buffer, data, BUFFERED_READ_SIZE)
    .onAny(lambda::bind(&os::close, fd));
}


Future<Nothing> write(int fd, const std::string& data)
{
  process::initialize();

  // Get our own copy of the file descriptor so that we're in control
  // of the lifetime and don't crash if/when someone by accidently
  // closes the file descriptor before discarding this future. We can
  // also make sure it's non-blocking and will close-on-exec. Start by
  // checking we've got a "valid" file descriptor before dup'ing.
  if (fd < 0) {
    return Failure(strerror(EBADF));
  }

  fd = dup(fd);
  if (fd == -1) {
    return Failure(ErrnoError("Failed to duplicate file descriptor"));
  }

  // Set the close-on-exec flag.
  Try<Nothing> cloexec = os::cloexec(fd);
  if (cloexec.isError()) {
    os::close(fd);
    return Failure(
        "Failed to set close-on-exec on duplicated file descriptor: " +
        cloexec.error());
  }

  // Make the file descriptor is non-blocking.
  Try<Nothing> nonblock = os::nonblock(fd);
  if (nonblock.isError()) {
    os::close(fd);
    return Failure(
        "Failed to make duplicated file descriptor non-blocking: " +
        nonblock.error());
  }

  return internal::_write(fd, Owned<string>(new string(data)), 0)
    .onAny(lambda::bind(&os::close, fd));
}


Future<Nothing> redirect(int from, Option<int> to, size_t chunk)
{
  // Make sure we've got "valid" file descriptors.
  if (from < 0 || (to.isSome() && to.get() < 0)) {
    return Failure(strerror(EBADF));
  }

  if (to.isNone()) {
    // Open up /dev/null that we can splice into.
    Try<int> open = os::open("/dev/null", O_WRONLY);

    if (open.isError()) {
      return Failure("Failed to open /dev/null for writing: " + open.error());
    }

    to = open.get();
  } else {
    // Duplicate 'to' so that we're in control of its lifetime.
    int fd = dup(to.get());
    if (fd == -1) {
      return Failure(ErrnoError("Failed to duplicate 'to' file descriptor"));
    }

    to = fd;
  }

  CHECK_SOME(to);

  // Duplicate 'from' so that we're in control of its lifetime.
  from = dup(from);
  if (from == -1) {
    return Failure(ErrnoError("Failed to duplicate 'from' file descriptor"));
  }

  // Set the close-on-exec flag (no-op if already set).
  Try<Nothing> cloexec = os::cloexec(from);
  if (cloexec.isError()) {
    os::close(from);
    os::close(to.get());
    return Failure("Failed to set close-on-exec on 'from': " + cloexec.error());
  }

  cloexec = os::cloexec(to.get());
  if (cloexec.isError()) {
    os::close(from);
    os::close(to.get());
    return Failure("Failed to set close-on-exec on 'to': " + cloexec.error());
  }

  // Make the file descriptors non-blocking (no-op if already set).
  Try<Nothing> nonblock = os::nonblock(from);
  if (nonblock.isError()) {
    os::close(from);
    os::close(to.get());
    return Failure("Failed to make 'from' non-blocking: " + nonblock.error());
  }

  nonblock = os::nonblock(to.get());
  if (nonblock.isError()) {
    os::close(from);
    os::close(to.get());
    return Failure("Failed to make 'to' non-blocking: " + nonblock.error());
  }

  return internal::splice(from, to.get(), chunk)
    .onAny(lambda::bind(&os::close, from))
    .onAny(lambda::bind(&os::close, to.get()));
}

} // namespace io {
} // namespace process {
