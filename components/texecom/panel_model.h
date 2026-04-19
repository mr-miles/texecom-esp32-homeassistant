#pragma once

// Abstract interface for panel-specific behaviour.
//
// Phase 1 ships only a placeholder Premier 24 implementation. Phase 2
// will extend this interface with decode()/encode()/message_ids. Phase
// 2+ Premier Elite variants (48/88/168/640) plug in as new concrete
// subclasses without touching the bridge.

#include <cstdint>

namespace esphome {
namespace texecom {

class PanelModel {
 public:
  virtual ~PanelModel() = default;

  // Human-readable panel name, e.g. "Premier 24".
  virtual const char *name() const = 0;

  // Number of zones this panel model supports.
  virtual uint8_t zone_count() const = 0;

  // Number of areas (partitions) this panel model supports.
  virtual uint8_t area_count() const = 0;

  // Future (Phase 2):
  //   virtual bool decode(const uint8_t *buf, size_t len, Event &out) = 0;
  //   virtual size_t encode(const Command &cmd, uint8_t *buf, size_t cap) = 0;
};

}  // namespace texecom
}  // namespace esphome
