#include "panel_model_premier24.h"

namespace esphome {
namespace texecom {

const char *PanelModelPremier24::name() const { return "Premier 24"; }

// Premier 24 has 24 zones. (Placeholder value — re-verify in Phase 2.)
uint8_t PanelModelPremier24::zone_count() const { return 24; }

// Premier 24 supports 2 areas / partitions. (Placeholder value — re-verify in Phase 2.)
uint8_t PanelModelPremier24::area_count() const { return 2; }

}  // namespace texecom
}  // namespace esphome
