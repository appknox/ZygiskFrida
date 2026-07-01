// Host unit tests for ZygiskFrida config parsing (no device / NDK required).
//
// Focus: the inject_on_specialize and enable_adb_bypasses target options —
// default, true, false, and invalid-type handling — plus a sanity check that
// the surrounding target fields still parse.
//
// Build & run:  module/src/jni/tests/run.sh

#include <sys/stat.h>

#include <cstdio>
#include <fstream>
#include <string>

#include "config.h"

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
  do {                                                                       \
    if (cond) {                                                             \
      std::printf("  ok   - %s\n", msg);                                    \
    } else {                                                                \
      std::printf("  FAIL - %s (%s:%d)\n", msg, __FILE__, __LINE__);        \
      ++g_failures;                                                         \
    }                                                                       \
  } while (0)

// Minimal valid target body (everything except inject_on_specialize).
static const char *BASE_TARGET =
    "\"app_name\": \"com.test.app\","
    "\"enabled\": true,"
    "\"start_up_delay_ms\": 0,"
    "\"injected_libraries\": [ { \"path\": \"/data/local/tmp/libknox.so\" } ]";

// Create a fresh per-case temp dir and write its config.json. Returns the dir.
static std::string write_config(const std::string &name, const std::string &body) {
  std::string dir = "/tmp/zygfri_cfg_test_" + name;
  mkdir(dir.c_str(), 0755);  // NOLINT — fine if it already exists
  std::ofstream(dir + "/config.json")
      << "{ \"targets\": [ {" << body << "} ] }";
  return dir;
}

int main() {
  std::printf("inject_on_specialize parsing:\n");

  // 1. Absent -> defaults to false, base fields still parse.
  {
    auto dir = write_config("absent", BASE_TARGET);
    auto cfg = load_config(dir, "com.test.app");
    CHECK(cfg.has_value(), "absent: config loads");
    CHECK(cfg && cfg->inject_on_specialize == false,
          "absent: inject_on_specialize defaults to false");
    CHECK(cfg && cfg->enabled && cfg->app_name == "com.test.app" &&
              cfg->start_up_delay_ms == 0 && cfg->injected_libraries.size() == 1,
          "absent: base target fields parsed");
  }

  // 2. Explicit true.
  {
    auto dir = write_config(
        "true", std::string(BASE_TARGET) + ",\"inject_on_specialize\": true");
    auto cfg = load_config(dir, "com.test.app");
    CHECK(cfg.has_value(), "true: config loads");
    CHECK(cfg && cfg->inject_on_specialize == true,
          "true: inject_on_specialize parsed as true");
  }

  // 3. Explicit false.
  {
    auto dir = write_config(
        "false", std::string(BASE_TARGET) + ",\"inject_on_specialize\": false");
    auto cfg = load_config(dir, "com.test.app");
    CHECK(cfg.has_value(), "false: config loads");
    CHECK(cfg && cfg->inject_on_specialize == false,
          "false: inject_on_specialize parsed as false");
  }

  // 4. Wrong type -> whole config rejected (no silent default).
  {
    auto dir = write_config(
        "wrongtype",
        std::string(BASE_TARGET) + ",\"inject_on_specialize\": \"yes\"");
    auto cfg = load_config(dir, "com.test.app");
    CHECK(!cfg.has_value(), "wrong-type: config rejected");
  }

  std::printf("enable_adb_bypasses parsing:\n");

  // 5. Absent -> defaults to false.
  {
    auto dir = write_config("adb_absent", BASE_TARGET);
    auto cfg = load_config(dir, "com.test.app");
    CHECK(cfg.has_value(), "adb absent: config loads");
    CHECK(cfg && cfg->enable_adb_bypasses == false,
          "adb absent: enable_adb_bypasses defaults to false");
  }

  // 6. Explicit true.
  {
    auto dir = write_config(
        "adb_true", std::string(BASE_TARGET) + ",\"enable_adb_bypasses\": true");
    auto cfg = load_config(dir, "com.test.app");
    CHECK(cfg.has_value(), "adb true: config loads");
    CHECK(cfg && cfg->enable_adb_bypasses == true,
          "adb true: enable_adb_bypasses parsed as true");
  }

  // 7. Explicit false.
  {
    auto dir = write_config(
        "adb_false",
        std::string(BASE_TARGET) + ",\"enable_adb_bypasses\": false");
    auto cfg = load_config(dir, "com.test.app");
    CHECK(cfg.has_value(), "adb false: config loads");
    CHECK(cfg && cfg->enable_adb_bypasses == false,
          "adb false: enable_adb_bypasses parsed as false");
  }

  // 8. Wrong type -> whole config rejected (no silent default).
  {
    auto dir = write_config(
        "adb_wrongtype",
        std::string(BASE_TARGET) + ",\"enable_adb_bypasses\": 1");
    auto cfg = load_config(dir, "com.test.app");
    CHECK(!cfg.has_value(), "adb wrong-type: config rejected");
  }

  if (g_failures == 0) {
    std::printf("\nALL PASSED\n");
    return 0;
  }
  std::printf("\n%d FAILURE(S)\n", g_failures);
  return 1;
}
