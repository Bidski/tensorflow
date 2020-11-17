/* Copyright 2015 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/core/common_runtime/device/device_id_manager.h"

#include <unordered_map>

#include "tensorflow/core/common_runtime/device/device_id.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/macros.h"
#include "tensorflow/core/platform/mutex.h"

namespace tensorflow {
namespace {
// Manages the map between TfDeviceId and platform device id.
class TfToPlatformDeviceIdMap {
 public:
  static TfToPlatformDeviceIdMap* singleton() {
    static auto* id_map = new TfToPlatformDeviceIdMap;
    return id_map;
  }

  Status Insert(const DeviceType& type, TfDeviceId tf_device_id,
                PlatformDeviceId platform_device_id) TF_LOCKS_EXCLUDED(mu_) {
    std::pair<IdMapType::iterator, bool> result;
    {
      mutex_lock lock(mu_);
      TypeIdMapType::iterator device_id_map_iter =
          id_map_.insert({type.type_string(), IdMapType()}).first;
      result = device_id_map_iter->second.insert(
          {tf_device_id.value(), platform_device_id.value()});
    }
    if (!result.second && platform_device_id.value() != result.first->second) {
      return errors::AlreadyExists(
          "TensorFlow device (", type, ":", tf_device_id.value(),
          ") is being mapped to multiple devices (", platform_device_id.value(),
          " now, and ", result.first->second,
          " previously), which is not supported. "
          "This may be the result of providing different ",
          type, " configurations (ConfigProto.gpu_options, for example ",
          "different visible_device_list) when creating multiple Sessions in ",
          "the same process. This is not currently supported, see ",
          "https://github.com/tensorflow/tensorflow/issues/19083");
    }
    return Status::OK();
  }

  bool Find(const DeviceType& type, TfDeviceId tf_device_id,
            PlatformDeviceId* platform_device_id) const TF_LOCKS_EXCLUDED(mu_) {
    // TODO(mrry): Consider replacing this with an atomic `is_initialized` bit,
    // to avoid writing to a shared cache line in the tf_shared_lock.
    tf_shared_lock lock(mu_);
    auto type_id_map_iter = id_map_.find(type.type_string());
    if (type_id_map_iter == id_map_.end()) return false;
    auto id_map_iter = type_id_map_iter->second.find(tf_device_id.value());
    if (id_map_iter == type_id_map_iter->second.end()) return false;
    *platform_device_id = id_map_iter->second;
    return true;
  }

 private:
  TfToPlatformDeviceIdMap() = default;

  void TestOnlyReset() TF_LOCKS_EXCLUDED(mu_) {
    mutex_lock lock(mu_);
    id_map_.clear();
  }

  // Map from physical device id to platform device id.
  using IdMapType = std::unordered_map<int32, int32>;
  // Map from DeviceType to IdMapType.
  // We use std::string instead of DeviceType because the key should
  // be default-initializable.
  using TypeIdMapType = std::unordered_map<std::string, IdMapType>;
  mutable mutex mu_;
  TypeIdMapType id_map_ TF_GUARDED_BY(mu_);

  friend class ::tensorflow::DeviceIdManager;
  TF_DISALLOW_COPY_AND_ASSIGN(TfToPlatformDeviceIdMap);
};
}  // namespace

Status DeviceIdManager::InsertTfPlatformDeviceIdPair(
    const DeviceType& type, TfDeviceId tf_device_id,
    PlatformDeviceId platform_device_id) {
  return TfToPlatformDeviceIdMap::singleton()->Insert(type, tf_device_id,
                                                      platform_device_id);
}

Status DeviceIdManager::TfToPlatformDeviceId(
    const DeviceType& type, TfDeviceId tf_device_id,
    PlatformDeviceId* platform_device_id) {
  if (TfToPlatformDeviceIdMap::singleton()->Find(type, tf_device_id,
                                                 platform_device_id)) {
    return Status::OK();
  }
  return errors::NotFound("TensorFlow device ", type, ":", tf_device_id.value(),
                          " was not registered");
}

void DeviceIdManager::TestOnlyReset() {
  TfToPlatformDeviceIdMap::singleton()->TestOnlyReset();
}

}  // namespace tensorflow
