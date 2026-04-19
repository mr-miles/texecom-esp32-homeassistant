#pragma once

#include "panel_model.h"

namespace esphome {
namespace texecom {

// Placeholder Premier 24 model. Zone/area counts are provisional; Phase 2
// verifies against a live panel and populates message IDs.
class PanelModelPremier24 : public PanelModel {
 public:
  const char *name() const override;
  uint8_t zone_count() const override;
  uint8_t area_count() const override;
};

}  // namespace texecom
}  // namespace esphome
