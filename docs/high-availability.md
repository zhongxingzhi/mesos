---
layout: documentation
---

# Mesos High Availability Mode

Mesos has a high-availability mode that uses multiple Mesos masters; one active master (called the leader or leading master) and several backups in case it fails. The masters elect the leader, with [Apache ZooKeeper](http://zookeeper.apache.org/) both coordinating the election and handling leader detection by masters, slaves, and scheduler drivers. More information regarding [how leader election works](http://zookeeper.apache.org/doc/trunk/recipes.html#sc_leaderElection) is available on the Apache Zookeeper website.

**Note**: This document assumes you know how to start, run, and work with ZooKeeper, whose client library is included in the standard Mesos build.

## Usage
To put Mesos into high-availability mode:

1. Ensure that the ZooKeeper cluster is up and running.

2. Provide the znode path to all masters, slaves, and framework schedulers as follows:

    * Start the mesos-master binaries using the `--zk` flag, e.g. `--zk=zk://host1:port1,host2:port2,.../path'

    * Start the mesos-slave binaries with `--master=zk://host1:port1,host2:port2,.../path`

    * Start any framework schedulers using the same `zk` path as in the last two steps. The SchedulerDriver must be constructed with this path, as shown in the [Framework Development Guide]( http://mesos.apache.org/documentation/latest/app-framework-development-guide/).

From now on, the Mesos masters and slaves all communicate with ZooKeeper to find out which master is the current leading master. This is in addition to the usual communication between the leading master and the slaves.

Refer to the [Scheduler API](http://mesos.apache.org/documentation/latest/app-framework-development-guide/) for how to deal with leadership changes.

## Implementation Details
Mesos implements two levels of ZooKeeper leader election abstractions, one in `src/zookeeper` and the other in `src/master` (look for `contender|detector.hpp|cpp`).

* The lower level `LeaderContender` and `LeaderDetector` implement a generic ZooKeeper election algorithm loosely modeled after this
[recipe](http://zookeeper.apache.org/doc/trunk/recipes.html#sc_leaderElection) (sans herd effect handling due to the master group's small size, which is often 3).

* The higher level `MasterContender` and `MasterDetector` wrap around ZooKeeper's contender and detector abstractions as adapters to provide/interpret the ZooKeeper data.

* Each Mesos master simultaneously uses both a contender and a detector to try to elect themselves and detect who the current leader is. A separate detector is necessary because each master's WebUI redirects browser traffic to the current leader when that master is not elected. Other Mesos components (i.e. slaves and scheduler drivers) use the detector to find the current leader and connect to it.

The notion of the group of leader candidates is implemented in `Group`. This abstraction handles reliable (through queues and retries of retryable errors under the covers) ZooKeeper group membership registration, cancellation, and monitoring. It watches for several ZooKeeper session events:

* Connection
* Reconnection
* Session Expiration
* ZNode creation, deletion, updates

We also explicitly timeout our sessions when disconnected from ZooKeeper for a specified amount of time. See `MASTER_CONTENDER_ZK_SESSION_TIMEOUT` and `MASTER_DETECTOR_ZK_SESSION_TIMEOUT`. This is because the ZooKeeper client libraries only notify of session expiration upon reconnection. These timeouts are of particular interest for network partitions.

## Component Disconnection Handling

When a network partition disconnects a component (master, slave, scheduler driver) from ZooKeeper, the component's Master Detector induces a timeout event. This notifies the component that it has no leading master. Depending on the component, the following happens. (Note that while a component is disconnected from ZooKeeper, a master may still be in communication with slaves or schedulers and vice versa.)

* Slaves disconnected from ZooKeeper no longer know which master is the leader. They ignore messages from masters to ensure they don't act on a non-leader's decisions. When a slave reconnects to ZooKeeper, ZooKeeper informs it of the current leader and the slave stops ignoring messages from the leader.

* Masters enter leaderless state irrespective of whether they are a leader or not before the disconnection.

    * If the leader was disconnected from ZooKeeper, it aborts its process. The user/developer/administrator can start a new, connected to ZooKeeper, master instance that starts as a backup.

    * Otherwise, the disconnected backup waits to reconnect with ZooKeeper and possibly get elected as the new leading master.

* Scheduler drivers disconnected from the leading master notify the scheduler about their disconnection from the leader.

When a network partition disconnects a slave from the leader:

* The slave fails health checks from the leader.

* The leader marks the slave as deactivated and sends its tasks to the LOST state. The  [Framework Development Guide](http://mesos.apache.org/documentation/latest/app-framework-development-guide/) describes these various task states.

* Deactivated slaves may not re-register with the leader, and are told to shut down upon any post-deactivation communication.
