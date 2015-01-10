#ifndef __PROCESS_FUTURE_HPP__
#define __PROCESS_FUTURE_HPP__

#include <assert.h>
#include <stdlib.h> // For abort.

#include <iostream>
#include <list>
#include <set>
#if  __cplusplus >= 201103L
#include <type_traits>
#endif // __cplusplus >= 201103L
#include <vector>

#include <glog/logging.h>

#if  __cplusplus < 201103L
#include <boost/type_traits.hpp>
#endif // __cplusplus < 201103L

#include <process/clock.hpp>
#include <process/internal.hpp>
#include <process/latch.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>
#include <process/timer.hpp>

#include <stout/abort.hpp>
#include <stout/duration.hpp>
#include <stout/error.hpp>
#include <stout/lambda.hpp>
#include <stout/memory.hpp> // TODO(benh): Replace shared_ptr with unique_ptr.
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/preprocessor.hpp>
#include <stout/try.hpp>

namespace process {

// Forward declaration (instead of include to break circular dependency).
template <typename _F>
struct _Defer;

template <typename F>
struct Deferred;

#if  __cplusplus >= 201103L
template <typename G>
struct _Deferred;
#endif // __cplusplus >= 201103L

template <typename T>
class Future;


namespace internal {

template <typename T>
struct wrap;

template <typename T>
struct unwrap;

} // namespace internal {


// Forward declaration of Promise.
template <typename T>
class Promise;


// Forward declaration of WeakFuture.
template <typename T>
class WeakFuture;

// Forward declaration of Failure.
struct Failure;


// Definition of a "shared" future. A future can hold any
// copy-constructible value. A future is considered "shared" because
// by default a future can be accessed concurrently.
template <typename T>
class Future
{
public:
  // Constructs a failed future.
  static Future<T> failed(const std::string& message);

  Future();

  /*implicit*/ Future(const T& _t);

  template <typename U>
  /*implicit*/ Future(const U& u);

  /*implicit*/ Future(const Failure& failure);

  /*implicit*/ Future(const Future<T>& that);

  /*implicit*/ Future(const Try<T>& t);

  ~Future();

  // Futures are assignable (and copyable). This results in the
  // reference to the previous future data being decremented and a
  // reference to 'that' being incremented.
  Future<T>& operator = (const Future<T>& that);

  // Comparision operators useful for using futures in collections.
  bool operator == (const Future<T>& that) const;
  bool operator != (const Future<T>& that) const;
  bool operator < (const Future<T>& that) const;

  // Helpers to get the current state of this future.
  bool isPending() const;
  bool isReady() const;
  bool isDiscarded() const;
  bool isFailed() const;
  bool hasDiscard() const;

  // Discards this future. Returns false if discard has already been
  // called or the future has already completed, i.e., is ready,
  // failed, or discarded. Note that a discard does not terminate any
  // computation but rather acts as a suggestion or indication that
  // the caller no longer cares about the result of some
  // computation. The callee can decide whether or not to continue the
  // computation on their own (where best practices are to attempt to
  // stop the computation if possible). The callee can discard the
  // computation via Promise::discard which completes a future, at
  // which point, Future::isDiscarded is true (and the
  // Future::onDiscarded callbacks are executed). Before that point,
  // but after calling Future::discard, only Future::hasDiscard will
  // return true and the Future::onDiscard callbacks will be invoked.
  bool discard();

  // Waits for this future to become ready, discarded, or failed.
  bool await(const Duration& duration = Seconds(-1)) const;

  // Return the value associated with this future, waits indefinitely
  // until a value gets associated or until the future is discarded.
  const T& get() const;

  // Returns the failure message associated with this future.
  const std::string& failure() const;

  // Type of the callback functions that can get invoked when the
  // future gets set, fails, or is discarded.
  typedef lambda::function<void(void)> DiscardCallback;
  typedef lambda::function<void(const T&)> ReadyCallback;
  typedef lambda::function<void(const std::string&)> FailedCallback;
  typedef lambda::function<void(void)> DiscardedCallback;
  typedef lambda::function<void(const Future<T>&)> AnyCallback;

#if __cplusplus >= 201103L
  // Installs callbacks for the specified events and returns a const
  // reference to 'this' in order to easily support chaining.
  const Future<T>& onDiscard(DiscardCallback&& callback) const;
  const Future<T>& onReady(ReadyCallback&& callback) const;
  const Future<T>& onFailed(FailedCallback&& callback) const;
  const Future<T>& onDiscarded(DiscardedCallback&& callback) const;
  const Future<T>& onAny(AnyCallback&& callback) const;

  // TODO(benh): Add onReady, onFailed, onAny for _Deferred<F> where F
  // is not expected.

  template <typename F>
  const Future<T>& onDiscard(_Deferred<F>&& deferred) const
  {
    return onDiscard(std::function<void()>(deferred));
  }

  template <typename F>
  const Future<T>& onReady(_Deferred<F>&& deferred) const
  {
    return onReady(std::function<void(const T&)>(deferred));
  }

  template <typename F>
  const Future<T>& onFailed(_Deferred<F>&& deferred) const
  {
    return onFailed(std::function<void(const std::string&)>(deferred));
  }

  template <typename F>
  const Future<T>& onDiscarded(_Deferred<F>&& deferred) const
  {
    return onDiscarded(std::function<void()>(deferred));
  }

  template <typename F>
  const Future<T>& onAny(_Deferred<F>&& deferred) const
  {
    return onAny(std::function<void(const Future<T>&)>(deferred));
  }

private:
  // We use the 'Prefer' and 'LessPrefer' structs as a way to prefer
  // one function over the other when doing SFINAE for the 'onReady',
  // 'onFailed', 'onAny', and 'then' functions. In each of these cases
  // we prefer calling the version of the functor that takes in an
  // argument (i.e., 'const T&' for 'onReady' and 'then' and 'const
  // std::string&' for 'onFailed'), but we allow functors that don't
  // care about the argument. We don't need to do this for
  // 'onDiscarded' because it doesn't take an argument.
  struct LessPrefer {};
  struct Prefer : LessPrefer {};

  template <typename F, typename = typename std::result_of<F(const T&)>::type>
  const Future<T>& onReady(F&& f, Prefer) const
  {
    return onReady(std::function<void(const T&)>(
        [=] (const T& t) mutable {
          f(t);
        }));
  }

  template <typename F, typename = typename std::result_of<F()>::type>
  const Future<T>& onReady(F&& f, LessPrefer) const
  {
    return onReady(std::function<void(const T&)>(
        [=] (const T&) mutable {
          f();
        }));
  }

  template <typename F, typename = typename std::result_of<F(const std::string&)>::type> // NOLINT(whitespace/line_length)
  const Future<T>& onFailed(F&& f, Prefer) const
  {
    return onFailed(std::function<void(const std::string&)>(
        [=] (const std::string& message) mutable {
          f(message);
        }));
  }

  template <typename F, typename = typename std::result_of<F()>::type>
  const Future<T>& onFailed(F&& f, LessPrefer) const
  {
    return onFailed(std::function<void(const std::string&)>(
        [=] (const std::string&) mutable {
          f();
        }));
  }

  template <typename F, typename = typename std::result_of<F(const Future<T>&)>::type> // NOLINT(whitespace/line_length)
  const Future<T>& onAny(F&& f, Prefer) const
  {
    return onAny(std::function<void(const Future<T>&)>(
        [=] (const Future<T>& future) mutable {
          f(future);
        }));
  }

  template <typename F, typename = typename std::result_of<F()>::type>
  const Future<T>& onAny(F&& f, LessPrefer) const
  {
    return onAny(std::function<void(const Future<T>&)>(
        [=] (const Future<T>&) mutable {
          f();
        }));
  }

public:
  template <typename F>
  const Future<T>& onDiscard(F&& f) const
  {
    return onDiscard(std::function<void()>(
        [=] () mutable {
          f();
        }));
  }

  template <typename F>
  const Future<T>& onReady(F&& f) const
  {
    return onReady(std::forward<F>(f), Prefer());
  }

  template <typename F>
  const Future<T>& onFailed(F&& f) const
  {
    return onFailed(std::forward<F>(f), Prefer());
  }

  template <typename F>
  const Future<T>& onDiscarded(F&& f) const
  {
    return onDiscarded(std::function<void()>(
        [=] () mutable {
          f();
        }));
  }

  template <typename F>
  const Future<T>& onAny(F&& f) const
  {
    return onAny(std::forward<F>(f), Prefer());
  }

#else // __cplusplus >= 201103L

  // Installs callbacks for the specified events and returns a const
  // reference to 'this' in order to easily support chaining.
  const Future<T>& onDiscard(const DiscardCallback& callback) const;
  const Future<T>& onReady(const ReadyCallback& callback) const;
  const Future<T>& onFailed(const FailedCallback& callback) const;
  const Future<T>& onDiscarded(const DiscardedCallback& callback) const;
  const Future<T>& onAny(const AnyCallback& callback) const;
#endif // __cplusplus >= 201103L

  // Installs callbacks that get executed when this future is ready
  // and associates the result of the callback with the future that is
  // returned to the caller (which may be of a different type).
  template <typename X>
  Future<X> then(const lambda::function<Future<X>(const T&)>& f) const;

  template <typename X>
  Future<X> then(const lambda::function<X(const T&)>& f) const;

  template <typename X>
  Future<X> then(const lambda::function<Future<X>()>& f) const
  {
    return then(lambda::function<Future<X>(const T&)>(lambda::bind(f)));
  }

  template <typename X>
  Future<X> then(const lambda::function<X()>& f) const
  {
    return then(lambda::function<X(const T&)>(lambda::bind(f)));
  }

  template <typename X>
  Future<X> then(const Deferred<Future<X>(T)>& f) const
  {
    return then(lambda::function<Future<X>(const T&)>(f));
  }

#if __cplusplus >= 201103L
private:
  template <typename F, typename X = typename internal::unwrap<typename std::result_of<F(const T&)>::type>::type> // NOLINT(whitespace/line_length)
  Future<X> then(_Deferred<F>&& f, Prefer) const
  {
    // note the then<X> is necessary to not have an infinite loop with
    // then(F&& f)
    return then<X>(std::function<Future<X>(const T&)>(f));
  }

  template <typename F, typename X = typename internal::unwrap<typename std::result_of<F()>::type>::type> // NOLINT(whitespace/line_length)
  Future<X> then(_Deferred<F>&& f, LessPrefer) const
  {
    return then<X>(std::function<Future<X>()>(f));
  }

  template <typename F, typename X = typename internal::unwrap<typename std::result_of<F(const T&)>::type>::type> // NOLINT(whitespace/line_length)
  Future<X> then(F&& f, Prefer) const
  {
    return then<X>(std::function<Future<X>(const T&)>(f));
  }

  template <typename F, typename X = typename internal::unwrap<typename std::result_of<F()>::type>::type> // NOLINT(whitespace/line_length)
  Future<X> then(F&& f, LessPrefer) const
  {
    return then<X>(std::function<Future<X>()>(f));
  }

public:
  template <typename F>
  auto then(F&& f) const
    -> decltype(this->then(std::forward<F>(f), Prefer()))
  {
    return then(std::forward<F>(f), Prefer());
  }

#else // __cplusplus >= 201103L

  // Helpers for the compiler to be able to forward std::tr1::bind results.
  template <typename X>
  Future<X> then(const std::tr1::_Bind<X(*(void))(void)>& b) const
  {
    return then(std::tr1::function<X(const T&)>(b));
  }

#define TEMPLATE(Z, N, DATA)                                            \
  template <typename X,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  Future<X> then(                                                       \
      const std::tr1::_Bind<X(*(ENUM_PARAMS(N, A)))                     \
      (ENUM_PARAMS(N, P))>& b) const                                    \
  {                                                                     \
    return then(std::tr1::function<X(const T&)>(b));                    \
  }

  REPEAT_FROM_TO(1, 11, TEMPLATE, _) // Args A0 -> A9.
#undef TEMPLATE

  template <typename X>
  Future<X> then(const std::tr1::_Bind<Future<X>(*(void))(void)>& b) const
  {
    return then(std::tr1::function<Future<X>(const T&)>(b));
  }

#define TEMPLATE(Z, N, DATA)                                            \
  template <typename X,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  Future<X> then(                                                       \
      const std::tr1::_Bind<Future<X>(*(ENUM_PARAMS(N, A)))             \
      (ENUM_PARAMS(N, P))>& b) const                                    \
  {                                                                     \
    return then(std::tr1::function<Future<X>(const T&)>(b));            \
  }

  REPEAT_FROM_TO(1, 11, TEMPLATE, _) // Args A0 -> A9.
#undef TEMPLATE

  // Helpers for the compiler to be able to forward 'defer' results.
  template <typename X, typename U>
  Future<X> then(const _Defer<Future<X>(*(PID<U>, X(U::*)(void)))
                 (const PID<U>&, X(U::*)(void))>& d) const
  {
    return then(std::tr1::function<Future<X>(const T&)>(d));
  }

#define TEMPLATE(Z, N, DATA)                                            \
  template <typename X,                                                 \
            typename U,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  Future<X> then(                                                       \
      const _Defer<Future<X>(*(PID<U>,                                  \
                               X(U::*)(ENUM_PARAMS(N, P)),              \
                               ENUM_PARAMS(N, A)))                      \
      (const PID<U>&,                                                   \
       X(U::*)(ENUM_PARAMS(N, P)),                                      \
       ENUM_PARAMS(N, P))>& d) const                                    \
  {                                                                     \
    return then(std::tr1::function<Future<X>(const T&)>(d));            \
  }

  REPEAT_FROM_TO(1, 11, TEMPLATE, _) // Args A0 -> A9.
#undef TEMPLATE

  template <typename X, typename U>
  Future<X> then(const _Defer<Future<X>(*(PID<U>, Future<X>(U::*)(void)))
                 (const PID<U>&, Future<X>(U::*)(void))>& d) const
  {
    return then(std::tr1::function<Future<X>(const T&)>(d));
  }

#define TEMPLATE(Z, N, DATA)                                            \
  template <typename X,                                                 \
            typename U,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  Future<X> then(                                                       \
      const _Defer<Future<X>(*(PID<U>,                                  \
                               Future<X>(U::*)(ENUM_PARAMS(N, P)),      \
                               ENUM_PARAMS(N, A)))                      \
      (const PID<U>&,                                                   \
       Future<X>(U::*)(ENUM_PARAMS(N, P)),                              \
       ENUM_PARAMS(N, P))>& d) const                                    \
  {                                                                     \
    return then(std::tr1::function<Future<X>(const T&)>(d));            \
  }

  REPEAT_FROM_TO(1, 11, TEMPLATE, _) // Args A0 -> A9.
#undef TEMPLATE
#endif // __cplusplus >= 201103L

  // Invokes the specified function after some duration if this future
  // has not been completed (set, failed, or discarded). Note that
  // this function is agnostic of discard semantics and while it will
  // propagate discarding "up the chain" it will still invoke the
  // specified callback after the specified duration even if 'discard'
  // was called on the returned future.
  Future<T> after(
      const Duration& duration,
      const lambda::function<Future<T>(const Future<T>&)>& f) const;

  // TODO(benh): Add overloads of 'after' that don't require passing
  // in a function that takes the 'const Future<T>&' parameter and use
  // Prefer/LessPrefer to disambiguate.

#if __cplusplus >= 201103L
  template <typename F>
  Future<T> after(
      const Duration& duration,
      _Deferred<F>&& f,
      typename std::enable_if<std::is_convertible<_Deferred<F>, std::function<Future<T>(const Future<T>&)>>::value>::type* = NULL) const // NOLINT(whitespace/line_length)
  {
    return after(duration, std::function<Future<T>(const Future<T>&)>(f));
  }

  template <typename F>
  Future<T> after(
      const Duration& duration,
      _Deferred<F>&& f,
      typename std::enable_if<std::is_convertible<_Deferred<F>, std::function<Future<T>()>>::value>::type* = NULL) const // NOLINT(whitespace/line_length)
  {
    return after(
        duration,
        std::function<Future<T>(const Future<T>&)>(std::bind(f)));
  }
#else
  template <typename F>
  Future<T> after(
      const Duration& duration,
      const _Defer<F>& f,
      typename boost::enable_if<boost::is_convertible<_Defer<F>, std::tr1::function<Future<T>(const Future<T>&)> > >::type* = NULL) const // NOLINT(whitespace/line_length)
  {
    return after(duration, std::tr1::function<Future<T>(const Future<T>&)>(f));
  }

  template <typename F>
  Future<T> after(
      const Duration& duration,
      const _Defer<F>& f,
      typename boost::enable_if<boost::is_convertible<_Defer<F>, std::tr1::function<Future<T>()> > >::type* = NULL) const // NOLINT(whitespace/line_length)
  {
    return after(
        duration,
        std::tr1::function<Future<T>(const Future<T>&)>(std::tr1::bind(f)));
  }
#endif // __cplusplus >= 201103L

private:
  friend class Promise<T>;
  friend class WeakFuture<T>;

  enum State
  {
    PENDING,
    READY,
    FAILED,
    DISCARDED,
  };

  struct Data
  {
    Data();
    ~Data();

    int lock;
    State state;
    bool discard;
    bool associated;
    T* t;
    std::string* message; // Message associated with failure.
    std::vector<DiscardCallback> onDiscardCallbacks;
    std::vector<ReadyCallback> onReadyCallbacks;
    std::vector<FailedCallback> onFailedCallbacks;
    std::vector<DiscardedCallback> onDiscardedCallbacks;
    std::vector<AnyCallback> onAnyCallbacks;
  };

  // Sets the value for this future, unless the future is already set,
  // failed, or discarded, in which case it returns false.
  bool set(const T& _t);

  // Sets this future as failed, unless the future is already set,
  // failed, or discarded, in which case it returns false.
  bool fail(const std::string& _message);

  memory::shared_ptr<Data> data;
};


namespace internal {

  // Helper for executing callbacks that have been registered.
  template <typename C, typename... Arguments>
  void run(const std::vector<C>& callbacks, Arguments&&... arguments)
  {
    for (size_t i = 0; i < callbacks.size(); ++i) {
      callbacks[i](std::forward<Arguments>(arguments)...);
    }
  }

} // namespace internal {


// Represents a weak reference to a future. This class is used to
// break cyclic dependencies between futures.
template <typename T>
class WeakFuture
{
public:
  explicit WeakFuture(const Future<T>& future);

  // Converts this weak reference to a concrete future. Returns none
  // if the conversion is not successful.
  Option<Future<T> > get() const;

private:
  memory::weak_ptr<typename Future<T>::Data> data;
};


template <typename T>
WeakFuture<T>::WeakFuture(const Future<T>& future)
  : data(future.data) {}


template <typename T>
Option<Future<T> > WeakFuture<T>::get() const
{
  Future<T> future;
  future.data = data.lock();

  if (future.data) {
    return future;
  }

  return None();
}


// Helper for creating failed futures.
struct Failure
{
  explicit Failure(const std::string& _message) : message(_message) {}
  explicit Failure(const Error& error) : message(error.message) {}

  const std::string message;
};


// Forward declaration to use as friend below.
namespace internal {
template <typename U>
void discarded(Future<U> future);
} // namespace internal {


// TODO(benh): Make Promise a subclass of Future?
template <typename T>
class Promise
{
public:
  Promise();
  explicit Promise(const T& t);
  virtual ~Promise();

  bool discard();
  bool set(const T& _t);
  bool set(const Future<T>& future); // Alias for associate.
  bool associate(const Future<T>& future);
  bool fail(const std::string& message);

  // Returns a copy of the future associated with this promise.
  Future<T> future() const;

private:
  template <typename U>
  friend void internal::discarded(Future<U> future);

  // Not copyable, not assignable.
  Promise(const Promise<T>&);
  Promise<T>& operator = (const Promise<T>&);

  // Helper for doing the work of actually discarding a future (called
  // from Promise::discard as well as internal::discarded).
  static bool discard(Future<T> future);

  Future<T> f;
};


template <>
class Promise<void>;


template <typename T>
class Promise<T&>;


namespace internal {

// Discards a weak future. If the weak future is invalid (i.e., the
// future it references to has already been destroyed), this operation
// is treated as a no-op.
template <typename T>
void discard(WeakFuture<T> reference)
{
  Option<Future<T> > future = reference.get();
  if (future.isSome()) {
    Future<T> future_ = future.get();
    future_.discard();
  }
}


// Helper for invoking Promise::discard in an onDiscarded callback
// (since the onDiscarded callback requires returning void we can't
// bind with Promise::discard).
template <typename T>
void discarded(Future<T> future)
{
  Promise<T>::discard(future);
}

} // namespace internal {


template <typename T>
Promise<T>::Promise() {}


template <typename T>
Promise<T>::Promise(const T& t)
  : f(t) {}


template <typename T>
Promise<T>::~Promise()
{
  // Note that we don't discard the promise as we don't want to give
  // the illusion that any computation hasn't started (or possibly
  // finished) in the event that computation is "visible" by other
  // means.
}


template <typename T>
bool Promise<T>::discard()
{
  if (!f.data->associated) {
    return discard(f);
  }
  return false;
}


template <typename T>
bool Promise<T>::set(const T& t)
{
  if (!f.data->associated) {
    return f.set(t);
  }
  return false;
}


template <typename T>
bool Promise<T>::set(const Future<T>& future)
{
  return associate(future);
}


template <typename T>
bool Promise<T>::associate(const Future<T>& future)
{
  bool associated = false;

  internal::acquire(&f.data->lock);
  {
    // Don't associate if this promise has completed. Note that this
    // does not include if Future::discard was called on this future
    // since in that case that would still leave the future PENDING
    // (note that we cover that case below).
    if (f.data->state == Future<T>::PENDING && !f.data->associated) {
      associated = f.data->associated = true;

      // After this point we don't allow 'f' to be completed via the
      // promise since we've set 'associated' but Future::discard on
      // 'f' might get called which will get propagated via the
      // 'f.onDiscard' below. Note that we currently don't propagate a
      // discard from 'future.onDiscard' but these semantics might
      // change if/when we make 'f' and 'future' true aliases of one
      // another.
    }
  }
  internal::release(&f.data->lock);

  // Note that we do the actual associating after releasing the lock
  // above to avoid deadlocking by attempting to require the lock
  // within from invoking 'f.onDiscard' and/or 'f.set/fail' via the
  // bind statements from doing 'future.onReady/onFailed'.
  if (associated) {
    // TODO(jieyu): Make 'f' a true alias of 'future'. Currently, only
    // 'discard' is associated in both directions. In other words, if
    // a future gets discarded, the other future will also get
    // discarded.  For 'set' and 'fail', they are associated only in
    // one direction.  In other words, calling 'set' or 'fail' on this
    // promise will not affect the result of the future that we
    // associated.
    f.onDiscard(lambda::bind(&internal::discard<T>, WeakFuture<T>(future)));

    future
      .onReady(lambda::bind(&Future<T>::set, f, lambda::_1))
      .onFailed(lambda::bind(&Future<T>::fail, f, lambda::_1))
      .onDiscarded(lambda::bind(&internal::discarded<T>, f));
  }

  return associated;
}


template <typename T>
bool Promise<T>::fail(const std::string& message)
{
  if (!f.data->associated) {
    return f.fail(message);
  }
  return false;
}


template <typename T>
Future<T> Promise<T>::future() const
{
  return f;
}


// Internal helper utilities.
namespace internal {

template <typename T>
struct wrap
{
  typedef Future<T> type;
};


template <typename X>
struct wrap<Future<X> >
{
  typedef Future<X> type;
};


template <typename T>
struct unwrap
{
  typedef T type;
};


template <typename X>
struct unwrap<Future<X> >
{
  typedef X type;
};


template <typename T>
void select(
    const Future<T>& future,
    memory::shared_ptr<Promise<Future<T > > > promise)
{
  // We never fail the future associated with our promise.
  assert(!promise->future().isFailed());

  if (promise->future().isPending()) { // No-op if it's discarded.
    if (future.isReady()) { // We only set the promise if a future is ready.
      promise->set(future);
    }
  }
}

} // namespace internal {


// TODO(benh): Move select and discard into 'futures' namespace.

// Returns a future that captures any ready future in a set. Note that
// select DOES NOT capture a future that has failed or been discarded.
template <typename T>
Future<Future<T> > select(const std::set<Future<T> >& futures)
{
  memory::shared_ptr<Promise<Future<T> > > promise(
      new Promise<Future<T> >());

  promise->future().onDiscard(
      lambda::bind(&internal::discarded<Future<T> >, promise->future()));

#if __cplusplus >= 201103L
  typename std::set<Future<T>>::iterator iterator;
  for (iterator = futures.begin(); iterator != futures.end(); ++iterator) {
    // NOTE: We can't use std::bind with a std::function with Clang
    // like we do below (see
    // http://stackoverflow.com/questions/20097616/stdbind-to-a-stdfunction-crashes-with-clang).
    (*iterator).onAny([=] (const Future<T>& future) {
      internal::select(future, promise);
    });
  }
#else // __cplusplus >= 201103L
  lambda::function<void(const Future<T>&)> select =
    lambda::bind(&internal::select<T>, lambda::_1, promise);

  typename std::set<Future<T> >::iterator iterator;
  for (iterator = futures.begin(); iterator != futures.end(); ++iterator) {
    (*iterator).onAny(lambda::bind(select, lambda::_1));
  }
#endif // __cplusplus >= 201103L

  return promise->future();
}


template <typename T>
void discard(const std::set<Future<T> >& futures)
{
  typename std::set<Future<T> >::const_iterator iterator;
  for (iterator = futures.begin(); iterator != futures.end(); ++iterator) {
    Future<T> future = *iterator; // Need a non-const copy to discard.
    future.discard();
  }
}


template <typename T>
void discard(const std::list<Future<T> >& futures)
{
  typename std::list<Future<T> >::const_iterator iterator;
  for (iterator = futures.begin(); iterator != futures.end(); ++iterator) {
    Future<T> future = *iterator; // Need a non-const copy to discard.
    future.discard();
  }
}


template <typename T>
bool Promise<T>::discard(Future<T> future)
{
  memory::shared_ptr<typename Future<T>::Data> data = future.data;

  bool result = false;

  internal::acquire(&data->lock);
  {
    if (data->state == Future<T>::PENDING) {
      data->state = Future<T>::DISCARDED;
      result = true;
    }
  }
  internal::release(&data->lock);

  // Invoke all callbacks associated with this future being
  // DISCARDED. We don't need a lock because the state is now in
  // DISCARDED so there should not be any concurrent modifications.
  if (result) {
    internal::run(future.data->onDiscardedCallbacks);
    future.data->onDiscardedCallbacks.clear();

    internal::run(future.data->onAnyCallbacks, future);
    future.data->onAnyCallbacks.clear();
  }

  return result;
}


template <typename T>
Future<T> Future<T>::failed(const std::string& message)
{
  Future<T> future;
  future.fail(message);
  return future;
}


template <typename T>
Future<T>::Data::Data()
  : lock(0),
    state(PENDING),
    discard(false),
    associated(false),
    t(NULL),
    message(NULL) {}


template <typename T>
Future<T>::Data::~Data()
{
  delete t;
  delete message;
}


template <typename T>
Future<T>::Future()
  : data(new Data()) {}


template <typename T>
Future<T>::Future(const T& _t)
  : data(new Data())
{
  set(_t);
}


template <typename T>
template <typename U>
Future<T>::Future(const U& u)
  : data(new Data())
{
  set(u);
}


template <typename T>
Future<T>::Future(const Failure& failure)
  : data(new Data())
{
  fail(failure.message);
}


template <typename T>
Future<T>::Future(const Future<T>& that)
  : data(that.data) {}


template <typename T>
Future<T>::Future(const Try<T>& t)
  : data(new Data())
{
  if (t.isSome()){
    set(t.get());
  } else {
    fail(t.error());
  }
}


template <typename T>
Future<T>::~Future() {}


template <typename T>
Future<T>& Future<T>::operator = (const Future<T>& that)
{
  if (this != &that) {
    data = that.data;
  }
  return *this;
}


template <typename T>
bool Future<T>::operator == (const Future<T>& that) const
{
  return data == that.data;
}


template <typename T>
bool Future<T>::operator != (const Future<T>& that) const
{
  return !(*this == that);
}


template <typename T>
bool Future<T>::operator < (const Future<T>& that) const
{
  return data < that.data;
}


template <typename T>
bool Future<T>::discard()
{
  bool result = false;

  internal::acquire(&data->lock);
  {
    if (!data->discard && data->state == PENDING) {
      result = data->discard = true;
    }
  }
  internal::release(&data->lock);

  // Invoke all callbacks associated with doing a discard on this
  // future. We don't need a lock because 'Data::discard' should now
  // be set so we won't be adding anything else to
  // 'Data::onDiscardCallbacks'.
  if (result) {
    internal::run(data->onDiscardCallbacks);
    data->onDiscardCallbacks.clear();
  }

  return result;
}


template <typename T>
bool Future<T>::isPending() const
{
  return data->state == PENDING;
}


template <typename T>
bool Future<T>::isReady() const
{
  return data->state == READY;
}


template <typename T>
bool Future<T>::isDiscarded() const
{
  return data->state == DISCARDED;
}


template <typename T>
bool Future<T>::isFailed() const
{
  return data->state == FAILED;
}


template <typename T>
bool Future<T>::hasDiscard() const
{
  return data->discard;
}


namespace internal {

inline void awaited(Owned<Latch> latch)
{
  latch->trigger();
}

} // namespace internal {


template <typename T>
bool Future<T>::await(const Duration& duration) const
{
  // NOTE: We need to preemptively allocate the Latch on the stack
  // instead of lazily create it in the critical section below because
  // instantiating a Latch requires creating a new process (at the
  // time of writing this comment) which might need to do some
  // synchronization in libprocess which might deadlock if some other
  // code in libprocess is already holding a lock and then attempts to
  // do Promise::set (or something similar) that attempts to acquire
  // the lock that we acquire here. This is an artifact of using
  // Future/Promise within the implementation of libprocess.
  //
  // We mostly only call 'await' in tests so this should not be a
  // performance concern.
  Owned<Latch> latch(new Latch());

  bool pending = false;

  internal::acquire(&data->lock);
  {
    if (data->state == PENDING) {
      pending = true;
      data->onAnyCallbacks.push_back(lambda::bind(&internal::awaited, latch));
    }
  }
  internal::release(&data->lock);

  if (pending) {
    return latch->await(duration);
  }

  return true;
}


template <typename T>
const T& Future<T>::get() const
{
  if (!isReady()) {
    await();
  }

  CHECK(!isPending()) << "Future was in PENDING after await()";
  // We can't use CHECK_READY here due to check.hpp depending on future.hpp.
  if (!isReady()) {
    CHECK(!isFailed()) << "Future::get() but state == FAILED: " << failure();
    CHECK(!isDiscarded()) << "Future::get() but state == DISCARDED";
  }

  assert(data->t != NULL);
  return *data->t;
}


template <typename T>
const std::string& Future<T>::failure() const
{
  if (data->state != FAILED) {
    ABORT("Future::failure() but state != FAILED");
  }
  return *(CHECK_NOTNULL(data->message));
}


#if __cplusplus >= 201103L
template <typename T>
const Future<T>& Future<T>::onDiscard(DiscardCallback&& callback) const
{
  bool run = false;

  internal::acquire(&data->lock);
  {
    if (data->discard) {
      run = true;
    } else if (data->state == PENDING) {
      data->onDiscardCallbacks.emplace_back(std::move(callback));
    }
  }
  internal::release(&data->lock);

  // TODO(*): Invoke callback in another execution context.
  if (run) {
    callback();
  }

  return *this;
}


template <typename T>
const Future<T>& Future<T>::onReady(ReadyCallback&& callback) const
{
  bool run = false;

  internal::acquire(&data->lock);
  {
    if (data->state == READY) {
      run = true;
    } else if (data->state == PENDING) {
      data->onReadyCallbacks.emplace_back(std::move(callback));
    }
  }
  internal::release(&data->lock);

  // TODO(*): Invoke callback in another execution context.
  if (run) {
    callback(*data->t);
  }

  return *this;
}


template <typename T>
const Future<T>& Future<T>::onFailed(FailedCallback&& callback) const
{
  bool run = false;

  internal::acquire(&data->lock);
  {
    if (data->state == FAILED) {
      run = true;
    } else if (data->state == PENDING) {
      data->onFailedCallbacks.emplace_back(std::move(callback));
    }
  }
  internal::release(&data->lock);

  // TODO(*): Invoke callback in another execution context.
  if (run) {
    callback(*data->message);
  }

  return *this;
}


template <typename T>
const Future<T>& Future<T>::onDiscarded(DiscardedCallback&& callback) const
{
  bool run = false;

  internal::acquire(&data->lock);
  {
    if (data->state == DISCARDED) {
      run = true;
    } else if (data->state == PENDING) {
      data->onDiscardedCallbacks.emplace_back(std::move(callback));
    }
  }
  internal::release(&data->lock);

  // TODO(*): Invoke callback in another execution context.
  if (run) {
    callback();
  }

  return *this;
}


template <typename T>
const Future<T>& Future<T>::onAny(AnyCallback&& callback) const
{
  bool run = false;

  internal::acquire(&data->lock);
  {
    if (data->state == PENDING) {
      data->onAnyCallbacks.emplace_back(std::move(callback));
    } else {
      run = true;
    }
  }
  internal::release(&data->lock);

  // TODO(*): Invoke callback in another execution context.
  if (run) {
    callback(*this);
  }

  return *this;
}

#else // __cplusplus >= 201103L
template <typename T>
const Future<T>& Future<T>::onDiscard(const DiscardCallback& callback) const
{
  bool run = false;

  internal::acquire(&data->lock);
  {
    if (data->discard) {
      run = true;
    } else if (data->state == PENDING) {
      data->onDiscardCallbacks.push_back(callback);
    }
  }
  internal::release(&data->lock);

  // TODO(*): Invoke callback in another execution context.
  if (run) {
    callback();
  }

  return *this;
}


template <typename T>
const Future<T>& Future<T>::onReady(const ReadyCallback& callback) const
{
  bool run = false;

  internal::acquire(&data->lock);
  {
    if (data->state == READY) {
      run = true;
    } else if (data->state == PENDING) {
      data->onReadyCallbacks.push_back(callback);
    }
  }
  internal::release(&data->lock);

  // TODO(*): Invoke callback in another execution context.
  if (run) {
    callback(*data->t);
  }

  return *this;
}


template <typename T>
const Future<T>& Future<T>::onFailed(const FailedCallback& callback) const
{
  bool run = false;

  internal::acquire(&data->lock);
  {
    if (data->state == FAILED) {
      run = true;
    } else if (data->state == PENDING) {
      data->onFailedCallbacks.push_back(callback);
    }
  }
  internal::release(&data->lock);

  // TODO(*): Invoke callback in another execution context.
  if (run) {
    callback(*data->message);
  }

  return *this;
}


template <typename T>
const Future<T>& Future<T>::onDiscarded(
    const DiscardedCallback& callback) const
{
  bool run = false;

  internal::acquire(&data->lock);
  {
    if (data->state == DISCARDED) {
      run = true;
    } else if (data->state == PENDING) {
      data->onDiscardedCallbacks.push_back(callback);
    }
  }
  internal::release(&data->lock);

  // TODO(*): Invoke callback in another execution context.
  if (run) {
    callback();
  }

  return *this;
}


template <typename T>
const Future<T>& Future<T>::onAny(const AnyCallback& callback) const
{
  bool run = false;

  internal::acquire(&data->lock);
  {
    if (data->state == PENDING) {
      data->onAnyCallbacks.push_back(callback);
    } else {
      run = true;
    }
  }
  internal::release(&data->lock);

  // TODO(*): Invoke callback in another execution context.
  if (run) {
    callback(*this);
  }

  return *this;
}
#endif // __cplusplus >= 201103L


namespace internal {

template <typename T, typename X>
void thenf(const memory::shared_ptr<Promise<X> >& promise,
           const lambda::function<Future<X>(const T&)>& f,
           const Future<T>& future)
{
  if (future.isReady()) {
    if (future.hasDiscard()) {
      promise->discard();
    } else {
      promise->associate(f(future.get()));
    }
  } else if (future.isFailed()) {
    promise->fail(future.failure());
  } else if (future.isDiscarded()) {
    promise->discard();
  }
}


template <typename T, typename X>
void then(const memory::shared_ptr<Promise<X> >& promise,
          const lambda::function<X(const T&)>& f,
          const Future<T>& future)
{
  if (future.isReady()) {
    if (future.hasDiscard()) {
      promise->discard();
    } else {
      promise->set(f(future.get()));
    }
  } else if (future.isFailed()) {
    promise->fail(future.failure());
  } else if (future.isDiscarded()) {
    promise->discard();
  }
}


template <typename T>
void expired(
    const lambda::function<Future<T>(const Future<T>&)>& f,
    const memory::shared_ptr<Latch>& latch,
    const memory::shared_ptr<Promise<T> >& promise,
    const Future<T>& future)
{
  if (latch->trigger()) {
    // Note that we don't bother checking if 'future' has been
    // discarded (i.e., 'future.isDiscarded()' returns true) since
    // there is a race between when we make that check and when we
    // would invoke 'f(future)' so the callee 'f' should ALWAYS check
    // if the future has been discarded and rather than hiding a
    // non-deterministic bug we always call 'f' if the timer has
    // expired.
    promise->associate(f(future));
  }
}


template <typename T>
void after(
    const memory::shared_ptr<Latch>& latch,
    const memory::shared_ptr<Promise<T> >& promise,
    const Timer& timer,
    const Future<T>& future)
{
  CHECK(!future.isPending());
  if (latch->trigger()) {
    Clock::cancel(timer);
    promise->associate(future);
  }
}

} // namespace internal {


template <typename T>
template <typename X>
Future<X> Future<T>::then(const lambda::function<Future<X>(const T&)>& f) const
{
  memory::shared_ptr<Promise<X> > promise(new Promise<X>());

  lambda::function<void(const Future<T>&)> thenf =
    lambda::bind(&internal::thenf<T, X>, promise, f, lambda::_1);

  onAny(thenf);

  // Propagate discarding up the chain. To avoid cyclic dependencies,
  // we keep a weak future in the callback.
  promise->future().onDiscard(
      lambda::bind(&internal::discard<T>, WeakFuture<T>(*this)));

  return promise->future();
}


template <typename T>
template <typename X>
Future<X> Future<T>::then(const lambda::function<X(const T&)>& f) const
{
  memory::shared_ptr<Promise<X> > promise(new Promise<X>());

  lambda::function<void(const Future<T>&)> then =
    lambda::bind(&internal::then<T, X>, promise, f, lambda::_1);

  onAny(then);

  // Propagate discarding up the chain. To avoid cyclic dependencies,
  // we keep a weak future in the callback.
  promise->future().onDiscard(
      lambda::bind(&internal::discard<T>, WeakFuture<T>(*this)));

  return promise->future();
}


template <typename T>
Future<T> Future<T>::after(
    const Duration& duration,
    const lambda::function<Future<T>(const Future<T>&)>& f) const
{
  // TODO(benh): Using a Latch here but Once might be cleaner.
  // Unfortunately, Once depends on Future so we can't easily use it
  // from here.
  memory::shared_ptr<Latch> latch(new Latch());
  memory::shared_ptr<Promise<T> > promise(new Promise<T>());

  // Set up a timer to invoke the callback if this future has not
  // completed. Note that we do not pass a weak reference for this
  // future as we don't want the future to get cleaned up and then
  // have the timer expire.
  Timer timer = Clock::timer(
      duration,
      lambda::bind(&internal::expired<T>, f, latch, promise, *this));

  onAny(lambda::bind(&internal::after<T>, latch, promise, timer, lambda::_1));

  // Propagate discarding up the chain. To avoid cyclic dependencies,
  // we keep a weak future in the callback.
  promise->future().onDiscard(
      lambda::bind(&internal::discard<T>, WeakFuture<T>(*this)));

  return promise->future();
}


template <typename T>
bool Future<T>::set(const T& _t)
{
  bool result = false;

  internal::acquire(&data->lock);
  {
    if (data->state == PENDING) {
      data->t = new T(_t);
      data->state = READY;
      result = true;
    }
  }
  internal::release(&data->lock);

  // Invoke all callbacks associated with this future being READY. We
  // don't need a lock because the state is now in READY so there
  // should not be any concurrent modications.
  if (result) {
    internal::run(data->onReadyCallbacks, *data->t);
    data->onReadyCallbacks.clear();

    internal::run(data->onAnyCallbacks, *this);
    data->onAnyCallbacks.clear();
  }

  return result;
}


template <typename T>
bool Future<T>::fail(const std::string& _message)
{
  bool result = false;

  internal::acquire(&data->lock);
  {
    if (data->state == PENDING) {
      data->message = new std::string(_message);
      data->state = FAILED;
      result = true;
    }
  }
  internal::release(&data->lock);

  // Invoke all callbacks associated with this future being FAILED. We
  // don't need a lock because the state is now in FAILED so there
  // should not be any concurrent modications.
  if (result) {
    internal::run(data->onFailedCallbacks, *data->message);
    data->onFailedCallbacks.clear();

    internal::run(data->onAnyCallbacks, *this);
    data->onAnyCallbacks.clear();
  }

  return result;
}

}  // namespace process {

#endif // __PROCESS_FUTURE_HPP__
