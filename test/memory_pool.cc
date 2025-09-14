#include "config.h"
#include "sephash.h"

int main(int argc, const char *argv[]) {
  Config config;
  config.ParseArg(argc, argv);
  SEPHASH::Server ser(config);
  while (true)
    ;
}
