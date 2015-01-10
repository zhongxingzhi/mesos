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
#ifndef __STOUT_PATH_HPP__
#define __STOUT_PATH_HPP__

#include <string>
#include <vector>

#include <stout/strings.hpp>

namespace path {

inline std::string join(const std::string& path1, const std::string& path2)
{
  return
    strings::remove(path1, "/", strings::SUFFIX) + "/" +
    strings::remove(path2, "/", strings::PREFIX);
}


inline std::string join(
    const std::string& path1,
    const std::string& path2,
    const std::string& path3)
{
  return join(path1, join(path2, path3));
}


inline std::string join(
    const std::string& path1,
    const std::string& path2,
    const std::string& path3,
    const std::string& path4)
{
  return join(path1, join(path2, path3, path4));
}


inline std::string join(
    const std::string& path1,
    const std::string& path2,
    const std::string& path3,
    const std::string& path4,
    const std::string& path5)
{
  return join(path1, join(path2, join(path3, join(path4, path5))));
}


inline std::string join(
    const std::string& path1,
    const std::string& path2,
    const std::string& path3,
    const std::string& path4,
    const std::string& path5,
    const std::string& path6)
{
  return join(path1, join(path2, path3, path4, path5, path6));
}


inline std::string join(const std::vector<std::string>& paths)
{
  if (paths.empty()) {
    return "";
  }

  std::string result = paths[0];
  for (size_t i = 1; i < paths.size(); ++i) {
    result = join(result, paths[i]);
  }
  return result;
}

} // namespace path {

#endif // __STOUT_PATH_HPP__
