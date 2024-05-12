#include "object_locker.h"

namespace esphome {
namespace energomera_iec {

std::vector<void *> AnyObjectLocker::locked_objects_(5);
Mutex AnyObjectLocker::lock_;

};  // namespace energomera_iec
};  // namespace esphome