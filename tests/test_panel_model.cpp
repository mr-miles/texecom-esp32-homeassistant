// Host-side unit tests for the PanelModel interface and the Premier 24
// placeholder implementation.

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <memory>

#include "panel_model.h"
#include "panel_model_premier24.h"

using esphome::texecom::PanelModel;
using esphome::texecom::PanelModelPremier24;

TEST_CASE("PanelModelPremier24: name() returns \"Premier 24\"", "[panel_model]") {
  PanelModelPremier24 m;
  REQUIRE(std::strcmp(m.name(), "Premier 24") == 0);
}

TEST_CASE("PanelModelPremier24: zone_count() returns 24", "[panel_model]") {
  PanelModelPremier24 m;
  REQUIRE(m.zone_count() == 24);
}

TEST_CASE("PanelModelPremier24: area_count() returns 2", "[panel_model]") {
  PanelModelPremier24 m;
  REQUIRE(m.area_count() == 2);
}

TEST_CASE("PanelModel: virtual dispatch via base pointer", "[panel_model]") {
  std::unique_ptr<PanelModel> p = std::make_unique<PanelModelPremier24>();
  REQUIRE(p != nullptr);
  REQUIRE(std::strcmp(p->name(), "Premier 24") == 0);
  REQUIRE(p->zone_count() == 24);
  REQUIRE(p->area_count() == 2);
}

// A tiny in-test subclass proves the interface is actually extensible
// for future Premier Elite variants — if this fails to compile, the
// abstraction has regressed.
namespace {
class FakeElite88 : public PanelModel {
 public:
  const char *name() const override { return "Fake Elite 88"; }
  uint8_t zone_count() const override { return 88; }
  uint8_t area_count() const override { return 8; }
};
}  // namespace

TEST_CASE("PanelModel: interface supports additional subclasses", "[panel_model]") {
  FakeElite88 e;
  PanelModel *base = &e;
  REQUIRE(std::strcmp(base->name(), "Fake Elite 88") == 0);
  REQUIRE(base->zone_count() == 88);
  REQUIRE(base->area_count() == 8);
}
