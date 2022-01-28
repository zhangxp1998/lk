/*
 * Copyright (c) 2021 Travis Geiseblrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

#include <sys/types.h>
#include <dev/bus/pci.h>
#include <lk/cpp.h>

#include "device.h"

namespace pci {

class bus;

// bridge device, holds a list of busses that it is responsible for
class bridge : public device {
public:
    bridge(pci_location_t loc, bus *bus);
    ~bridge() override;

    DISALLOW_COPY_ASSIGN_AND_MOVE(bridge);

    static status_t probe(pci_location_t loc, bus *parent_bus, bridge **out_bridge);

    // called when a sub bridge is assigned a new secondary bus.
    // if this extends our subordinate bus, recursively call the parent bridge.
    void extend_subordinate_range(uint8_t new_secondary_bus);

    void assign_bus_numbers(uint8_t primary, uint8_t secondary, uint8_t subordinate_bus);

    void add_bus(bus *b) { secondary_bus_ = b; }

    void dump(size_t indent = 0) override;

    // config accessors
    uint8_t primary_bus() const { return config_.type1.primary_bus; }
    uint8_t secondary_bus() const { return config_.type1.secondary_bus; }
    uint8_t subordinate_bus() const { return config_.type1.subordinate_bus; }

    template <typename T>
    struct range {
        T base;
        T limit;
    };

    range<uint32_t> io_range();
    range<uint32_t> mem_range();
    range<uint64_t> prefetch_range();

private:
    bus *secondary_bus_ = nullptr;
};

} // pci

