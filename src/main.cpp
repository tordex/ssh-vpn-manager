#include <adwaita.h>

#include "app.hpp"

int main(int argc, char** argv) {
  adw_init();
  const auto app = App::create();
  return app->run(argc, argv);
}
