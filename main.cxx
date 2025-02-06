
#include "server.hpp"
#include <filesystem>
#include <format>

int main() { // HTTP

  server svr;
  svr.listen("127.0.0.1", 8808);
  svr.run();
}