#include "core/optimize/Executor.h"

#include <new>
#include <stdexcept>

namespace ac {

Executor::~Executor() = default;

const char* python_exception_name(const std::exception& e)
{
   if (dynamic_cast<const std::invalid_argument*>(&e) || dynamic_cast<const std::domain_error*>(&e) ||
       dynamic_cast<const std::length_error*>(&e))
      return "ValueError";
   if (dynamic_cast<const std::out_of_range*>(&e))
      return "IndexError";
   if (dynamic_cast<const std::bad_alloc*>(&e))
      return "MemoryError";
   if (dynamic_cast<const std::overflow_error*>(&e) || dynamic_cast<const std::underflow_error*>(&e))
      return "OverflowError";
   return "RuntimeError";
}

std::string describe_executor_failure(const std::exception& e)
{
   return std::string(python_exception_name(e)) + ": " + e.what();
}

std::string ImageStore::put(std::shared_ptr<const Image> image, std::string id_hint)
{
   if (!image)
      throw std::invalid_argument("ImageStore::put: null image for id '" + id_hint + "'");
   if (images_.count(id_hint) != 0)
      throw std::invalid_argument("ImageStore::put: id '" + id_hint + "' is already in use");
   images_.emplace(id_hint, std::move(image));
   return id_hint;
}

std::shared_ptr<const Image> ImageStore::get(const std::string& id) const
{
   const auto it = images_.find(id);
   if (it == images_.end())
      throw std::out_of_range("ImageStore::get: no image with id '" + id + "'");
   return it->second;
}

void ImageStore::release_unreferenced(const std::set<std::string>& live)
{
   for (auto it = images_.begin(); it != images_.end();) {
      if (live.count(it->first) == 0)
         it = images_.erase(it);
      else
         ++it;
   }
}

} // namespace ac
