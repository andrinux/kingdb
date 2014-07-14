// Copyright (c) 2014, Emmanuel Goossaert. All rights reserved.
// Use of this source code is governed the BSD 3-Clause License,
// that can be found in the LICENSE file.

#include "event_manager.h"

namespace kdb {

Event<std::vector<Order>> EventManager::flush_buffer;
Event<std::map<std::string, uint64_t>> EventManager::update_index;
Event<int> EventManager::clear_buffer;

};