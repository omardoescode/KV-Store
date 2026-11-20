#include "cp/resolve.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_name_resolves_to_ipv4(void) {
  char addr[DRIVER_ADDR_LEN];
  assert(resolve_addr("localhost:7000", addr) == 0);
  assert(strcmp(addr, "127.0.0.1:7000") == 0);
  printf("  test_name_resolves_to_ipv4 OK\n");
}

static void test_ip_stays(void) {
  char addr[DRIVER_ADDR_LEN];
  assert(resolve_addr("10.0.0.7:7890", addr) == 0);
  assert(strcmp(addr, "10.0.0.7:7890") == 0);
  printf("  test_ip_stays OK\n");
}

static void test_no_port_fails(void) {
  char addr[DRIVER_ADDR_LEN];
  assert(resolve_addr("localhost", addr) == -1);
  printf("  test_no_port_fails OK\n");
}

int main(void) {
  printf("resolve tests:\n");
  test_name_resolves_to_ipv4();
  test_ip_stays();
  test_no_port_fails();
  printf("all resolve tests passed\n");
  return 0;
}
