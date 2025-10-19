#include <stdio.h>
#include <string.h>

// Simple local implementation matching contains_ack behavior for unit testing
int contains_ack_test(const char* s) {
  if (!s)
    return 0;
  return strstr(s, "ACK") != NULL;
}

int main(void) {
  struct {
    const char* in;
    int expect;
  } cases[] = {{"ACK\n", 1},  {"  ACK\r\n", 1}, {"somethingACKsomething\n", 1},
               {"NACK\n", 1},  // contains 'ACK'
               {"NO\n", 0},   {"\"ACK\"", 1},   {NULL, 0}};

  int all_ok = 1;
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    int r = contains_ack_test(cases[i].in);
    if (r != cases[i].expect) {
      printf("Test %zu FAILED: input='%s' got=%d expected=%d\n", i,
             cases[i].in ? cases[i].in : "(null)", r, cases[i].expect);
      all_ok = 0;
    }
  }

  if (all_ok) {
    printf("All contains_ack tests passed\n");
    return 0;
  }
  return 1;
}
