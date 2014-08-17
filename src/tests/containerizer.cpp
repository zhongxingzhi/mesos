/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "tests/containerizer.hpp"
#include "tests/mesos.hpp"

using std::map;
using std::string;

using testing::_;
using testing::Invoke;
using testing::Return;

using namespace process;

namespace mesos {
namespace internal {
namespace tests {


TestContainerizer::TestContainerizer(
    const hashmap<ExecutorID, Executor*>& _executors)
  : executors(_executors)
{
  setup();
}


TestContainerizer::TestContainerizer(
    const ExecutorID& executorId,
    Executor* executor)
{
  executors[executorId] = executor;
  setup();
}


TestContainerizer::TestContainerizer(MockExecutor* executor)
{
  executors[executor->id] = executor;
  setup();
}


TestContainerizer::TestContainerizer()
{
  setup();
}


TestContainerizer::~TestContainerizer()
{
  foreachvalue (const Owned<MesosExecutorDriver>& driver, drivers) {
    driver->stop();
    driver->join();
  }
  drivers.clear();
}


Future<bool> TestContainerizer::_launch(
    const ContainerID& containerId,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& user,
    const SlaveID& slaveId,
    const PID<slave::Slave>& slavePid,
    bool checkpoint)
{
  CHECK(!drivers.contains(containerId))
    << "Failed to launch executor " << executorInfo.executor_id()
    << " of framework " << executorInfo.framework_id()
    << " because it is already launched";

  CHECK(executors.contains(executorInfo.executor_id()))
    << "Failed to launch executor " << executorInfo.executor_id()
    << " of framework " << executorInfo.framework_id()
    << " because it is unknown to the containerizer";

  // Store mapping from (frameworkId, executorId) -> containerId to facilitate
  // easy destroy from tests.
  std::pair<FrameworkID, ExecutorID> key(executorInfo.framework_id(),
                                         executorInfo.executor_id());
  containers_[key] = containerId;

  Executor* executor = executors[executorInfo.executor_id()];
  Owned<MesosExecutorDriver> driver(new MesosExecutorDriver(executor));
  drivers[containerId] = driver;

  // Prepare additional environment variables for the executor.
  const map<string, string>& env = executorEnvironment(
      executorInfo,
      directory,
      slaveId,
      slavePid,
      checkpoint,
      Duration::zero());

  foreachpair (const string& name, const string variable, env) {
    os::setenv(name, variable);
  }

  foreach (const Environment_Variable& variable,
      executorInfo.command().environment().variables()) {
    os::setenv(variable.name(), variable.value());
  }
  os::setenv("MESOS_LOCAL", "1");

  driver->start();

  foreachkey (const string& name, env) {
    os::unsetenv(name);
  }

  foreach(const Environment_Variable& variable,
      executorInfo.command().environment().variables()) {
    os::unsetenv(variable.name());
  }
  os::unsetenv("MESOS_LOCAL");

  Owned<Promise<containerizer::Termination> > promise(
      new Promise<containerizer::Termination>());
  promises[containerId] = promise;

  return true;
}


Future<bool> TestContainerizer::launch(
    const ContainerID& containerId,
    const TaskInfo& taskInfo,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& user,
    const SlaveID& slaveId,
    const PID<slave::Slave>& slavePid,
    bool checkpoint)
{
  return launch(
      containerId,
      executorInfo,
      directory,
      user,
      slaveId,
      slavePid,
      checkpoint);
}


Future<containerizer::Termination> TestContainerizer::_wait(
    const ContainerID& containerId)
{
  // An unknown container is possible for tests where we "drop" the
  // 'launch' in order to verify recovery still works correctly.
  if (!promises.contains(containerId)) {
    return Failure("Unknown container: " + stringify(containerId));
  }

  return promises[containerId]->future();
}


void TestContainerizer::destroy(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId)
{
  std::pair<FrameworkID, ExecutorID> key(frameworkId, executorId);
  if (!containers_.contains(key)) {
    LOG(WARNING) << "Ignoring destroy of unknown container for executor '"
                  << executorId << "' of framework '" << frameworkId << "'";
    return;
  }
  destroy(containers_[key]);
}


void TestContainerizer::destroy(const ContainerID& containerId)
{
  CHECK(drivers.contains(containerId))
    << "Failed to terminate container " << containerId
    << " because it is has not been started";

  Owned<MesosExecutorDriver> driver = drivers[containerId];
  driver->stop();
  driver->join();
  drivers.erase(containerId);

  containerizer::Termination termination;
  termination.set_killed(false);
  termination.set_message("Killed executor");
  termination.set_status(0);
  promises[containerId]->set(termination);
  promises.erase(containerId);
}


Future<hashset<ContainerID> > TestContainerizer::containers()
{
  return promises.keys();
}


void TestContainerizer::setup()
{
  // NOTE: We use 'EXPECT_CALL' and 'WillRepeatedly' here instead of
  // 'ON_CALL' and 'WillByDefault' because the latter gives the gmock
  // warning "Uninteresting mock function call" unless each tests puts
  // the expectations in place which would make the tests much more
  // verbose.
  // See groups.google.com/forum/#!topic/googlemock/EX4kLxddlko for a
  // suggestion for how we might be able to use 'NiceMock' here
  // instead.
  EXPECT_CALL(*this, recover(_))
    .WillRepeatedly(Return(Nothing()));

  EXPECT_CALL(*this, usage(_))
    .WillRepeatedly(Return(ResourceStatistics()));

  EXPECT_CALL(*this, update(_, _))
    .WillRepeatedly(Return(Nothing()));

  EXPECT_CALL(*this, launch(_, _, _, _, _, _, _))
    .WillRepeatedly(Invoke(this, &TestContainerizer::_launch));

  EXPECT_CALL(*this, wait(_))
    .WillRepeatedly(Invoke(this, &TestContainerizer::_wait));
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
