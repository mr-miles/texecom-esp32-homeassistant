// Host-side unit tests for capture_http.h's `is_safe_capture_filename()`.
//
// Plan 02-01 fix-up. Path-safety is the only security-relevant piece of
// the HTTP handler — the actual handler class is gated behind USE_ARDUINO
// and exercised on-device. These tests pin down the validator's behaviour
// so a future refactor can't accidentally widen what's accepted.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "capture_http.h"

using esphome::texecom::is_safe_capture_filename;

TEST_CASE("capture_http: plain capture filenames pass", "[capture_http][path]") {
  REQUIRE(is_safe_capture_filename("wintex-1700000000-001.bin"));
  REQUIRE(is_safe_capture_filename("wintex-1700000000-001.txt"));
  REQUIRE(is_safe_capture_filename("wintex-boot12345-002.txt"));
  REQUIRE(is_safe_capture_filename("wintex-boot12345-002.bin"));
  // Mixed case extension still accepted.
  REQUIRE(is_safe_capture_filename("foo.BIN"));
  REQUIRE(is_safe_capture_filename("foo.Txt"));
}

TEST_CASE("capture_http: traversal attempts are rejected", "[capture_http][path]") {
  REQUIRE_FALSE(is_safe_capture_filename(".."));
  REQUIRE_FALSE(is_safe_capture_filename("."));
  REQUIRE_FALSE(is_safe_capture_filename("../etc/passwd"));
  REQUIRE_FALSE(is_safe_capture_filename("..\\windows"));
  REQUIRE_FALSE(is_safe_capture_filename("foo/../bar.bin"));
  REQUIRE_FALSE(is_safe_capture_filename("./foo.bin"));
  // Hidden / dotfiles are out by the leading-dot rule.
  REQUIRE_FALSE(is_safe_capture_filename(".secret.bin"));
  REQUIRE_FALSE(is_safe_capture_filename(".bin"));
}

TEST_CASE("capture_http: path separators are rejected", "[capture_http][path]") {
  REQUIRE_FALSE(is_safe_capture_filename("foo/bar.bin"));
  REQUIRE_FALSE(is_safe_capture_filename("\\windows\\system32.bin"));
  REQUIRE_FALSE(is_safe_capture_filename("a/b.bin"));
  REQUIRE_FALSE(is_safe_capture_filename("a\\b.bin"));
  REQUIRE_FALSE(is_safe_capture_filename("/captures/foo.bin"));
  // Windows drive specifier banned defensively.
  REQUIRE_FALSE(is_safe_capture_filename("C:foo.bin"));
}

TEST_CASE("capture_http: control bytes are rejected", "[capture_http][path]") {
  std::string with_null = "foo.bin";
  with_null += '\0';
  with_null += "tail";
  REQUIRE_FALSE(is_safe_capture_filename(with_null));

  std::string with_newline = "foo\nbar.bin";
  REQUIRE_FALSE(is_safe_capture_filename(with_newline));

  std::string with_tab = "foo\tbar.bin";
  REQUIRE_FALSE(is_safe_capture_filename(with_tab));

  std::string with_cr = "foo\rbar.bin";
  REQUIRE_FALSE(is_safe_capture_filename(with_cr));

  // DEL (0x7F) — not technically a control char but treated as one.
  std::string with_del;
  with_del += "foo";
  with_del += static_cast<char>(0x7F);
  with_del += ".bin";
  REQUIRE_FALSE(is_safe_capture_filename(with_del));
}

TEST_CASE("capture_http: empty / oversize names are rejected", "[capture_http][path]") {
  REQUIRE_FALSE(is_safe_capture_filename(""));
  // Right at the boundary — 96 chars is allowed, 97 is not.
  std::string at_limit(92, 'a');
  at_limit += ".bin";  // total 96 chars
  REQUIRE(at_limit.size() == 96);
  REQUIRE(is_safe_capture_filename(at_limit));
  std::string over_limit(93, 'a');
  over_limit += ".bin";  // total 97 chars
  REQUIRE(over_limit.size() == 97);
  REQUIRE_FALSE(is_safe_capture_filename(over_limit));
}

TEST_CASE("capture_http: non-bin/txt extensions are rejected",
          "[capture_http][path]") {
  REQUIRE_FALSE(is_safe_capture_filename("foo"));
  REQUIRE_FALSE(is_safe_capture_filename("foo.exe"));
  REQUIRE_FALSE(is_safe_capture_filename("foo.html"));
  REQUIRE_FALSE(is_safe_capture_filename("foo.json"));
  // No extension at all.
  REQUIRE_FALSE(is_safe_capture_filename("README"));
  // Truncated extension (".bi" / ".tx") must fail.
  REQUIRE_FALSE(is_safe_capture_filename("foo.bi"));
  REQUIRE_FALSE(is_safe_capture_filename("foo.tx"));
  // A 4-char filename like "x.bin" is the minimum legal length (5).
  REQUIRE(is_safe_capture_filename("x.bin"));
  REQUIRE_FALSE(is_safe_capture_filename(".bin"));  // leading dot fails first
}
