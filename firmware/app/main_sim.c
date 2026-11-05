#include "app.h"
#include "semihosting.h"

int main(void) {
  sh_write0("\n=== OPNA Studio P2 (QEMU netduino2) ===\n");
  studio_app_sim();
  sh_exit(0);
  return 0;
}
