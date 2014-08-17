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

#ifndef __LINUX_ROUTING_FILTER_ICMP_HPP__
#define __LINUX_ROUTING_FILTER_ICMP_HPP__

#include <string>
#include <vector>

#include <stout/net.hpp>
#include <stout/option.hpp>
#include <stout/result.hpp>
#include <stout/try.hpp>

#include "linux/routing/filter/action.hpp"
#include "linux/routing/filter/filter.hpp"
#include "linux/routing/filter/priority.hpp"

#include "linux/routing/queueing/handle.hpp"

namespace routing {
namespace filter {
namespace icmp {

class Classifier
{
public:
  explicit Classifier(const Option<net::IP>& _destinationIP)
    : destinationIP_(_destinationIP) {}

  bool operator == (const Classifier& that) const
  {
    return destinationIP_ == that.destinationIP_;
  }

  const Option<net::IP>& destinationIP() const { return destinationIP_; }

private:
  Option<net::IP> destinationIP_;
};


// Returns true if there exists an ICMP packet filter attached to the
// given parent on the link which matches the specified classifier.
Try<bool> exists(
    const std::string& link,
    const queueing::Handle& parent,
    const Classifier& classifier);


// Creates an ICMP packet filter attached to the given parent on the
// link which will redirect all the ICMP packets that satisfy the
// conditions specified by the classifier to the target link. Returns
// false if an ICMP packet filter attached to the given parent with
// the same classifier already exists. The user can choose to specify
// an optional priority for the filter.
Try<bool> create(
    const std::string& link,
    const queueing::Handle& parent,
    const Classifier& classifier,
    const Option<Priority>& priority,
    const action::Redirect& redirect);


// Creates an ICMP packet filter attached to the given parent on the
// link which will mirror all the ICMP packets that satisfy the
// conditions specified by the classifier to a set of links (specified
// in the mirror action). Returns false if an ICMP packet filter
// attached to the given parent with the same classifier already
// exists. The user can choose to specify an optional priority for the
// filter.
Try<bool> create(
    const std::string& link,
    const queueing::Handle& parent,
    const Classifier& classifier,
    const Option<Priority>& priority,
    const action::Mirror& mirror);


// Removes the ICMP packet filter attached to the given parent that
// matches the specified classifier from the link. Returns false if
// such a filter is not found.
Try<bool> remove(
    const std::string& link,
    const queueing::Handle& parent,
    const Classifier& classifier);


// Updates the action of the ICMP packet filter attached to the given
// parent that matches the specified classifier on the link. Returns
// false if such a filter is not found.
Try<bool> update(
    const std::string& link,
    const queueing::Handle& parent,
    const Classifier& classifier,
    const action::Mirror& mirror);


// Returns the classifiers of all the ICMP packet filters attached to
// the given parent on the link. Returns None if the link or the
// parent is not found.
Result<std::vector<Classifier> > classifiers(
    const std::string& link,
    const queueing::Handle& parent);

} // namespace icmp {
} // namespace filter {
} // namespace routing {

#endif // __LINUX_ROUTING_FILTER_ICMP_HPP__
