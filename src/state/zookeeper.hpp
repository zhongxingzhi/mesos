#ifndef __STATE_ZOOKEEPER_HPP__
#define __STATE_ZOOKEEPER_HPP__

#include <set>
#include <string>

#include <process/future.hpp>

#include <stout/duration.hpp>
#include <stout/option.hpp>
#include <stout/uuid.hpp>

#include "messages/state.hpp"

#include "state/storage.hpp"

#include "zookeeper/authentication.hpp"

namespace mesos {
namespace internal {
namespace state {

// Forward declarations.
class ZooKeeperStorageProcess;


class ZooKeeperStorage : public Storage
{
public:
  // TODO(benh): Just take a zookeeper::URL.
  ZooKeeperStorage(
      const std::string& servers,
      const Duration& timeout,
      const std::string& znode,
      const Option<zookeeper::Authentication>& auth = None());
  virtual ~ZooKeeperStorage();

  // Storage implementation.
  virtual process::Future<Option<Entry> > get(const std::string& name);
  virtual process::Future<bool> set(const Entry& entry, const UUID& uuid);
  virtual process::Future<bool> expunge(const Entry& entry);
  virtual process::Future<std::set<std::string> > names();

private:
  ZooKeeperStorageProcess* process;
};


} // namespace state {
} // namespace internal {
} // namespace mesos {

#endif // __STATE_ZOOKEEPER_HPP__
