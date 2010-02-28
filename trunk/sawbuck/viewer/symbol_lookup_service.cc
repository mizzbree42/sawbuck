// Copyright 2010 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Symbol lookup service implementation.
#include "sawbuck/viewer/symbol_lookup_service.h"

#include <algorithm>
#include "base/message_loop.h"

template <>
struct RunnableMethodTraits<SymbolLookupService> {
  RunnableMethodTraits() {
  }

  ~RunnableMethodTraits() {
  }

  void RetainCallee(SymbolLookupService* obj) {
  }

  void ReleaseCallee(SymbolLookupService* obj) {
  }
};


SymbolLookupService::SymbolLookupService() : background_thread_(NULL),
    foreground_thread_(MessageLoop::current()), resolve_task_(NULL),
    callback_task_(NULL), next_request_id_(0), unprocessed_id_(0) {
}

bool SymbolLookupService::ResolveAddress(sym_util::ProcessId pid,
                                         const base::Time& time,
                                         sym_util::Address address,
                                         sym_util::Symbol* symbol) {
  // Hold the symbol cache lock over the whole shebang.
  AutoLock lock(symbol_lock_);

  using sym_util::ModuleCache;
  using sym_util::SymbolCache;

  ModuleCache::ModuleLoadStateId id;
  SymbolCacheMap::iterator it;

  {
    // Hold the module lock only while accessing the module cache.
    AutoLock lock(module_lock_);

    id = module_cache_.GetStateId(pid, time);
    it = symbol_caches_.find(id);
    if (it == symbol_caches_.end()) {
      // We have a miss, initialize a cache for this module id.
      if (symbol_caches_.size() == kMaxCacheSize) {
        // Evict the least recently used element.
        ModuleCache::ModuleLoadStateId to_evict = lru_module_id_.front();
        lru_module_id_.erase(lru_module_id_.begin());
        symbol_caches_.erase(to_evict);
      }

      std::pair<SymbolCacheMap::iterator, bool> inserted =
          symbol_caches_.insert(std::make_pair(id, SymbolCache()));

      DCHECK_EQ(inserted.second, true);
      SymbolCache& cache = inserted.first->second;

      std::vector<ModuleInformation> modules;
      module_cache_.GetProcessModuleState(pid, time, &modules);
      cache.Initialize(modules.size(), modules.size() ? &modules[0] : NULL);

      it = inserted.first;
    } else {
      // We have a hit, manage the LRU by removing our ID.
      // It will be pushed to the back of the LRU just below.
      lru_module_id_.erase(
          std::find(lru_module_id_.begin(), lru_module_id_.end(), id));
    }
  }

  // Push our id to the back of the lru list.
  lru_module_id_.push_back(id);

  DCHECK(it != symbol_caches_.end());
  SymbolCache& cache = it->second;

  // This can take a long time, so it's important not to
  // hold the module lock over this operation.
  return cache.GetSymbolForAddress(address, symbol);
}

SymbolLookupService::Handle SymbolLookupService::ResolveAddress(
    sym_util::ProcessId process_id, const base::Time& time,
    sym_util::Address address, SymbolResolvedCallback* callback) {
  DCHECK_EQ(foreground_thread_, MessageLoop::current());
  DCHECK(callback != NULL);

  AutoLock lock(symbol_lock_);
  Handle request_id = next_request_id_++;
  DCHECK(requests_.end() == requests_.find(request_id));
  Request& request = requests_[request_id];
  request.process_id_ = process_id;
  request.time_ = time;
  request.address_ = address;
  request.callback_ = callback;

  if (!resolve_task_) {
    resolve_task_ = NewRunnableMethod(this,
                                      &SymbolLookupService::ResolveCallback);
    background_thread_->PostTask(FROM_HERE, resolve_task_);
  }

  return request_id;
}

void SymbolLookupService::CancelRequest(Handle request_handle) {
  DCHECK_EQ(foreground_thread_, MessageLoop::current());
  AutoLock lock(symbol_lock_);

  RequestMap::iterator it = requests_.find(request_handle);
  DCHECK(it != requests_.end());
  delete it->second.callback_;
  requests_.erase(it);
}

void SymbolLookupService::OnModuleIsLoaded(
    DWORD process_id, const base::Time& time,
    const ModuleInformation& module_info) {
  return OnModuleLoad(process_id, time, module_info);
}

void SymbolLookupService::OnModuleUnload(
    DWORD process_id, const base::Time& time,
    const ModuleInformation& module_info) {
  AutoLock lock(module_lock_);
  module_cache_.ModuleUnloaded(process_id, time, module_info);
}

void SymbolLookupService::OnModuleLoad(
    DWORD process_id, const base::Time& time,
    const ModuleInformation& module_info) {
  std::wstring file_path(module_info.image_file_name);
  // Map device paths to drive paths.
  DWORD drives = ::GetLogicalDrives();
  char drive = 'A';
  for (; drives != 0; drives >>= 1, ++drive) {
    if (drives & 1) {
      wchar_t device_path[1024] = {};
      wchar_t device[] = { drive, L':', L'\0' };
      if (::QueryDosDevice(device, device_path, arraysize(device_path)) &&
          file_path.find(device_path) == 0) {
        std::wstring new_path = device;
        new_path += file_path.substr(wcslen(device_path));
        file_path = new_path;
      }
    }
  }

  ModuleInformation& info = const_cast<ModuleInformation&>(module_info);
  info.image_file_name = file_path;

  AutoLock lock(module_lock_);

  module_cache_.ModuleLoaded(process_id, time, module_info);
}

void SymbolLookupService::ResolveCallback() {
  while (true) {
    Handle request_id;
    Request request;

    {
      AutoLock lock(symbol_lock_);

      RequestMap::iterator it = requests_.lower_bound(unprocessed_id_);
      if (it == requests_.end()) {
        resolve_task_ = NULL;
        return;
      }

      request_id = it->first;
      request = it->second;
    }

    sym_util::Symbol symbol;
    ResolveAddress(request.process_id_,
                   request.time_,
                   request.address_,
                   &symbol);

    {
      AutoLock lock(symbol_lock_);

      RequestMap::iterator it = requests_.find(request_id);
      if (it != requests_.end()) {
        it->second.resolved_ = symbol;

        if (!callback_task_) {
          callback_task_ = NewRunnableMethod(
              this, &SymbolLookupService::IssueCallbacks);
          foreground_thread_->PostTask(FROM_HERE, callback_task_);
        }
      }

      unprocessed_id_ = request_id + 1;
    }
  }
}

void SymbolLookupService::IssueCallbacks() {
  while (true) {
    Request request;
    Handle request_id;

    // Find the lowest request that has been processed.
    {
      AutoLock lock(symbol_lock_);

      RequestMap::iterator it = requests_.begin();
      if (it == requests_.end() || it->first >= unprocessed_id_) {
        callback_task_ = NULL;
        return;
      }

      request_id = it->first;
      request = it->second;

      requests_.erase(it);
    }

    request.callback_->Run(request.process_id_,
                           request.time_,
                           request.address_,
                           request_id,
                           request.resolved_);

    delete request.callback_;
  }
}
