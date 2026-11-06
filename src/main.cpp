#include <adwaita.h>

#include <curl/curl.h>

#include "app.hpp"

int main(int argc, char** argv)
{
    adw_init();
    curl_global_init(CURL_GLOBAL_DEFAULT);
    const auto app = App::create();
    const int  rc  = app->run(argc, argv);
    curl_global_cleanup();
    return rc;
}
