#include "config.h"
#include "sephash.h"

int main(int argc, char *argv[]) {
  Config config;
  config.ParseArg(argc, argv);
  SEPHASH::Server ser(config);
  while (true)
    ;
}
