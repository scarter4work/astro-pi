// AutoContrast core (PCL-free): the Executor seam and the in-process image store — optimize/executor.py.
//
// The loop never touches pixels. It emits an Action; an Executor realises it. The module's executor runs real
// PixInsight processes; tests use ApproxExecutor (a port of the Python NumpyExecutor). Loop logic is identical
// under both.
#pragma once

#include "core/Image.h"
#include "core/optimize/Actions.h"

#include <exception>
#include <map>
#include <memory>
#include <set>
#include <string>

namespace ac {

class Executor {
public:
   virtual ~Executor();
   // A new image with `action` applied to `parent` (never mutates it). A candidate failure must be thrown as a
   // std::exception: the loop catches exactly std::exception, logs it as an `executor` entry for that candidate
   // only and goes on with the next action (§12). Anything that must END the run — a user Cancel
   // (pcl::ProcessAborted) and every other PCL exception, none of which derives from std::exception — is not
   // caught and escapes advance(); an implementation translates a per-candidate PCL error into a std::exception
   // itself and lets aborts through.
   virtual Image apply(const Image& parent, const Action& action, double pixel_scale_arcsec) = 0;
};

// The Python exception class name a C++ exception is reported under, so the loop's log reads exactly as
// loop.py's `f"{type(exc).__name__}: {exc}"`. The mapping follows the convention ac_core uses everywhere for
// errors ported from Python:
//    std::invalid_argument, std::domain_error   -> ValueError   (NumpyExecutor raises ValueError)
//    std::out_of_range                          -> IndexError
//    std::length_error                          -> ValueError
//    std::bad_alloc                             -> MemoryError
//    std::overflow_error, std::underflow_error  -> OverflowError
//    std::range_error, std::runtime_error, std::logic_error and any other std::exception -> RuntimeError
//    (including std::system_error / std::ios_base::failure, which Python would name OSError; no executor in
//    ac_core throws them)
// (Order matters: the most derived classes are tested first.)
const char* python_exception_name(const std::exception& e);

// "<python_exception_name>: <what()>" — the `reason` of an executor-failure log entry.
std::string describe_executor_failure(const std::exception& e);

// Images addressed by id, held by shared pointer: the module runs at a 1600 px proxy with ~13 live images, so a
// candidate nobody references any more must be freed, not kept until the end of the run.
class ImageStore {
public:
   // Stores `image` under exactly `id_hint` and returns it (the loop's ids are Python's candidate paths, so the
   // id is never rewritten). Throws std::invalid_argument for a null image or an id already in use: the loop
   // builds ids that are unique by construction, so a collision is a bug, never something to overwrite.
   std::string put(std::shared_ptr<const Image> image, std::string id_hint);
   // Throws std::out_of_range naming the id when it is not stored.
   std::shared_ptr<const Image> get(const std::string& id) const;
   bool contains(const std::string& id) const { return images_.count(id) != 0; }
   size_t size() const { return images_.size(); }
   // Drops every image whose id is not in `live`. (A shared_ptr still held elsewhere keeps its pixels alive.)
   void release_unreferenced(const std::set<std::string>& live);

private:
   std::map<std::string, std::shared_ptr<const Image>> images_;
};

} // namespace ac
