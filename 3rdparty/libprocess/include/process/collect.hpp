#ifndef __PROCESS_COLLECT_HPP__
#define __PROCESS_COLLECT_HPP__

#include <list>

#include <process/check.hpp>
#include <process/defer.hpp>
#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>

#include <stout/lambda.hpp>
#include <stout/tuple.hpp>

// TODO(bmahler): Move these into a futures.hpp header to group Future
// related utilities.

namespace process {

// Waits on each future in the specified list and returns the list of
// resulting values in the same order. If any future is discarded then
// the result will be a failure. Likewise, if any future fails then
// the result future will be a failure.
template <typename T>
Future<std::list<T> > collect(const std::list<Future<T> >& futures);


// Waits on each future in the specified set and returns the list of
// non-pending futures.
template <typename T>
Future<std::list<Future<T> > > await(const std::list<Future<T> >& futures);


// Waits on each future specified and returns the wrapping future
// typed of a tuple of futures.
template <typename T1, typename T2>
Future<tuples::tuple<Future<T1>, Future<T2> > > await(
    const Future<T1>& future1,
    const Future<T2>& future2);


namespace internal {

template <typename T>
class CollectProcess : public Process<CollectProcess<T> >
{
public:
  CollectProcess(
      const std::list<Future<T> >& _futures,
      Promise<std::list<T> >* _promise)
    : futures(_futures),
      promise(_promise),
      ready(0) {}

  virtual ~CollectProcess()
  {
    delete promise;
  }

  virtual void initialize()
  {
    // Stop this nonsense if nobody cares.
    promise->future().onDiscard(defer(this, &CollectProcess::discarded));

    typename std::list<Future<T> >::const_iterator iterator;
    for (iterator = futures.begin(); iterator != futures.end(); ++iterator) {
      (*iterator).onAny(defer(this, &CollectProcess::waited, lambda::_1));
    }
  }

private:
  void discarded()
  {
    promise->discard();
    terminate(this);
  }

  void waited(const Future<T>& future)
  {
    if (future.isFailed()) {
      promise->fail("Collect failed: " + future.failure());
      terminate(this);
    } else if (future.isDiscarded()) {
      promise->fail("Collect failed: future discarded");
      terminate(this);
    } else {
      CHECK_READY(future);
      ready += 1;
      if (ready == futures.size()) {
        std::list<T> values;
        foreach (const Future<T>& future, futures) {
          values.push_back(future.get());
        }
        promise->set(values);
        terminate(this);
      }
    }
  }

  const std::list<Future<T> > futures;
  Promise<std::list<T> >* promise;
  size_t ready;
};


template <typename T>
class AwaitProcess : public Process<AwaitProcess<T> >
{
public:
  AwaitProcess(
      const std::list<Future<T> >& _futures,
      Promise<std::list<Future<T> > >* _promise)
    : futures(_futures),
      promise(_promise),
      ready(0) {}

  virtual ~AwaitProcess()
  {
    delete promise;
  }

  virtual void initialize()
  {
    // Stop this nonsense if nobody cares.
    promise->future().onDiscard(defer(this, &AwaitProcess::discarded));

    typename std::list<Future<T> >::const_iterator iterator;
    for (iterator = futures.begin(); iterator != futures.end(); ++iterator) {
      (*iterator).onAny(defer(this, &AwaitProcess::waited, lambda::_1));
    }
  }

private:
  void discarded()
  {
    promise->discard();
    terminate(this);
  }

  void waited(const Future<T>& future)
  {
    CHECK(!future.isPending());

    ready += 1;
    if (ready == futures.size()) {
      promise->set(futures);
      terminate(this);
    }
  }

  const std::list<Future<T> > futures;
  Promise<std::list<Future<T> > >* promise;
  size_t ready;
};


#if __cplusplus < 201103L

template <typename T>
void _await(
    const Future<T>& result,
    Owned<Promise<Nothing> > promise)
{
  promise->set(Nothing());
}


template <typename T1, typename T2>
Future<tuples::tuple<Future<T1>, Future<T2> > > __await(
    const tuples::tuple<Future<T1>, Future<T2> >& future)
{
  return future;
}

#endif // __cplusplus < 201103L

} // namespace internal {


template <typename T>
inline Future<std::list<T> > collect(
    const std::list<Future<T> >& futures)
{
  if (futures.empty()) {
    return std::list<T>();
  }

  Promise<std::list<T> >* promise = new Promise<std::list<T> >();
  Future<std::list<T> > future = promise->future();
  spawn(new internal::CollectProcess<T>(futures, promise), true);
  return future;
}


template <typename T>
inline Future<std::list<Future<T> > > await(
    const std::list<Future<T> >& futures)
{
  if (futures.empty()) {
    return futures;
  }

  Promise<std::list<Future<T> > >* promise =
    new Promise<std::list<Future<T> > >();
  Future<std::list<Future<T> > > future = promise->future();
  spawn(new internal::AwaitProcess<T>(futures, promise), true);
  return future;
}


#if __cplusplus >= 201103L

template <typename T1, typename T2>
Future<tuples::tuple<Future<T1>, Future<T2>>> await(
    const Future<T1>& future1,
    const Future<T2>& future2)
{
  Owned<Promise<Nothing> > promise1(new Promise<Nothing>());

  future1.onAny([=] () { promise1->set(Nothing()); });

  Owned<Promise<Nothing> > promise2(new Promise<Nothing>());

  future2.onAny([=] () { promise2->set(Nothing()); });

  std::list<Future<Nothing> > futures;
  futures.push_back(promise1->future());
  futures.push_back(promise2->future());

  return await(futures)
    .then([=] () { return tuples::make_tuple(future1, future2); });
}

#else // __cplusplus >= 201103L

template <typename T1, typename T2>
Future<tuples::tuple<Future<T1>, Future<T2> > > await(
    const Future<T1>& future1,
    const Future<T2>& future2)
{
  Owned<Promise<Nothing> > promise1(new Promise<Nothing>());

  future1.onAny(lambda::bind(internal::_await<T1>, lambda::_1, promise1));

  Owned<Promise<Nothing> > promise2(new Promise<Nothing>());

  future2.onAny(lambda::bind(internal::_await<T2>, lambda::_1, promise2));

  std::list<Future<Nothing> > futures;
  futures.push_back(promise1->future());
  futures.push_back(promise2->future());

  return await(futures)
    .then(lambda::bind(
      internal::__await<T1, T2>,
      tuples::make_tuple(future1, future2)));
}

#endif // __cplusplus >= 201103L

} // namespace process {

#endif // __PROCESS_COLLECT_HPP__
