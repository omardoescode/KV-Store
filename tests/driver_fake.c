#include <stdio.h>
#include <string.h>

#include "cp/driver.h"
#include "cp_fake.h"

enum { FAKE_MAX_ID = 15 };

static char fault_phase[16];
static int fault_victim = -1;

void fake_fault(const char *phase, int victim) {
  (void)snprintf(fault_phase, sizeof(fault_phase), "%s", phase);
  fault_victim = victim;
}

int driver_init(const cp_config *cfg, logger log) {
  (void)cfg;
  (void)log;
  return 0;
}

int driver_spawn(int id, char addr[DRIVER_ADDR_LEN]) {
  if (id < 1 || id > FAKE_MAX_ID) {
    return -1;
  }
  fake_start(id);
  (void)snprintf(addr, DRIVER_ADDR_LEN, "%s", fake_addr(id));
  return 0;
}

int driver_stop(int id) {
  fake_kill(id);
  return 0;
}

int driver_list(driver_node *out, size_t max) {
  size_t n = 0;
  for (int id = 1; id <= FAKE_MAX_ID && n != max; id++) {
    if (fake_running(id)) {
      out[n].id = id;
      (void)snprintf(out[n].addr, DRIVER_ADDR_LEN, "%s", fake_addr(id));
      n++;
    }
  }
  return (int)n;
}

void driver_fault_point(const char *phase, int target) {
  if (fault_victim < 0 || strcmp(phase, fault_phase) != 0) {
    return;
  }
  fake_kill(fault_victim == 0 ? target : fault_victim);
  fault_victim = -1;
}
