// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BASE_SAMPLING_HEAP_PROFILER_MODULE_CACHE_H_
#define BASE_SAMPLING_HEAP_PROFILER_MODULE_CACHE_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/base_export.h"
#include "base/files/file_path.h"
#include "build/build_config.h"

#if defined(OS_WIN)
#include <windows.h>
#endif

namespace base {

class BASE_EXPORT ModuleCache {
 public:
  // Module represents a binary module (executable or library) and its
  // associated state.
  class BASE_EXPORT Module {
   public:
    Module(uintptr_t base_address,
           const std::string& id,
           const FilePath& filename);
    Module(uintptr_t base_address,
           const std::string& id,
           const FilePath& filename,
           size_t size);
    ~Module();

    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;

    // Gets the base address of the module.
    uintptr_t GetBaseAddress() const;

    // Gets the opaque binary string that uniquely identifies a particular
    // program version with high probability. This is parsed from headers of the
    // loaded module.
    // For binaries generated by GNU tools:
    //   Contents of the .note.gnu.build-id field.
    // On Windows:
    //   GUID + AGE in the debug image headers of a module.
    std::string GetId() const;

    // Gets the filename of the module.
    // TODO(wittman): This is really the debug basename of the file, which is
    // the pdb basename for Windows and the binary basename for other
    // platforms. Update the method name accordingly.
    FilePath GetFilename() const;

    // Gets the size of the module.
    size_t GetSize() const;

   private:
    uintptr_t base_address_;
    std::string id_;
    FilePath filename_;
    size_t size_;
  };

  ModuleCache();
  ~ModuleCache();

  // Gets the module containing |address| or nullptr if |address| is not within
  // a module. The returned module remains owned by and has the same lifetime as
  // the ModuleCache object.
  const Module* GetModuleForAddress(uintptr_t address);
  std::vector<const Module*> GetModules() const;

 private:
  // TODO(alph): Refactor corresponding functions to use public API instead,
  // and drop friends.

  // Creates a Module object for the specified memory address. Returns null if
  // the address does not belong to a module.
  static std::unique_ptr<Module> CreateModuleForAddress(uintptr_t address);
  friend class NativeStackSamplerMac;

#if defined(OS_MACOSX)
  // Returns the size of the _TEXT segment of the module loaded
  // at |module_addr|.
  static size_t GetModuleTextSize(const void* module_addr);
  friend bool MayTriggerUnwInitLocalCrash(uint64_t);
#endif

#if defined(OS_WIN)
  const Module* GetModuleForHandle(HMODULE module_handle);
  static std::unique_ptr<Module> CreateModuleForHandle(HMODULE module_handle);
  friend class NativeStackSamplerWin;

  // The module objects, indexed by the module handle.
  // TODO(wittman): Merge this state into modules_cache_map_ and remove
  std::map<HMODULE, std::unique_ptr<Module>> win_module_cache_;
#endif

  std::map<uintptr_t, std::unique_ptr<Module>> modules_cache_map_;
};

}  // namespace base

#endif  // BASE_SAMPLING_HEAP_PROFILER_MODULE_CACHE_H_
